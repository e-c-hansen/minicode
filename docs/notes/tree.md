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
  after the drop (Finder waits on a modal during it). **Rows dragged
  inside the app** (2026-11-18, the grading walker on 1.6.3): the
  within-app mask is Move, Copy and Generic, so the tree's own folders
  take a move and the terminal (which types the path) and the editor's
  text (`CodeTextView onDropFiles`, `openDroppedPaths:`: files open in
  kept tabs, folders are revealed) take a copy; to Finder it stays Copy.
  `dragBegan` is set in ClickOutline's `beginDraggingSessionWithItems:`
  override, which the tracking calls before it returns, and when the
  tracking returns on a drag event, not only in the delegate's
  `willBeginAtPoint:`. Checked in terminal_paste: mouse events posted to
  the outline's own tracking, the session's items and mask caught at
  `beginDraggingSessionWithItems:` (the window is never on screen), then
  dropped on the grid and the text view. Trap: a within-app mask of Move
  alone had the terminal refuse every row (it takes only Copy, Generic or
  Link), and #187's check never saw it because its fake drag offered
  every operation; a drop check names the real source's mask. `pathMovedFrom:to:` moves
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
- **Icons by kind** (2026-11-15, replaced on the Mac by types
  2026-11-18): `FileIcon::kindOf` (FileDetails, by extension only,
  `tree:icons`; the image, video and audio tables there are the ones
  `isImagePath:`/`isMediaPath:` read) still says what a click opens.
  `MCTreeSymbolImage` (TreeDetails.mm) makes the folders' and worktree
  rows' symbols once each and falls back to doc.text for a symbol the
  system lacks. The git colors tint the name, never the icon. **Icons by type**
  (2026-11-18, the owner on the GTK port: "almost none of the different
  file types ... have non-default icons"; `linux/HANDOFF.md` item 109):
  the core's `FileIcon::typeOf` (FileDetails, `tree:icon-types`) is finer
  than `Kind`: 78 types (C, C header, C++, Python, Rust, Go, ... YAML,
  TOML, LaTeX, fonts, executables, lockfiles, git's files, config,
  Makefile, Dockerfile, LICENSE, README), by the whole name first
  (`typesByName`, then the stems LICENSE, README, AUTHORS and Dockerfile
  when what follows is no extension or a text one, so license_check.py
  stays Python), then the extension (`typesByExtension`), then a dotfile
  is Config and a file with no extension the caller found executable is
  Executable, else Code or Text as `kindOf` would say. `Kind` stays what a
  click opens (the session web page's "kind" reads it). The GTK port maps
  each type to icon names from the system theme (`linux/src/TreeIcons.cpp`:
  the freedesktop mimetype names first, text-x-python, application-json,
  text-x-makefile, then broader ones, text-x-script, then text-x-generic),
  the first the display's theme has (`gtk_icon_theme_has_icon`, answers
  kept per theme and forgotten on its "changed", when `FileTree::
  refreshIcons` gives the rows on show the new names). File icons are the
  theme's in colour; folders keep the tinted folder-symbolic. On Yaru 77
  of the 78 types draw apart from plain text, in 65 pictures (the
  `tree_icons` suite counts them by the image file each name draws); on
  Adwaita, GTK's default and the harness's, 17 names cover them (it lacks
  the languages: they fall back to text-x-script). Names no theme on the
  ThinkPad has: Swift, Julia, Elixir, Erlang, Zig and OCaml exist in
  Papirus only; a lockfile is Yaru's legacy object-locked, git's files
  Papirus's git or else text-x-patch, config Yaru's and Papirus's
  configure. The GTK listing asks `access(X_OK)` of a regular file with no
  extension only (on its worker thread: `minicode-exec` on the GFileInfo),
  so a listing still reads no other file's details; a chmod +x shows when
  the row is listed anew. Trap (GTK): a name the icon theme lacks draws GTK's missing-image
  picture (no fallback by dropping parts of the name), so every name is
  checked with `gtk_icon_theme_has_icon` and each type's list ends with
  text-x-generic.
- **The Mac's icons by type** (2026-11-18, `FileTypeIcons.{h,mm}`, the
  `tree` suite): `MCFileTypeOf` (the core's `typeOf`, with the row's
  `executable`: `MCTreeStat` notes a regular file's execute bit, as the
  GTK listing does), `MCFileTypeImage` and `MCFileTypeTint`. Each type in
  `kLooks` is an SF Symbol, a space-separated list tried in order (so a
  symbol newer than macOS 11, such as cylinder or cup.and.saucer, falls
  back to the next, and no `@available` is needed), or, for a language SF
  Symbols has no picture of, its short name drawn in the bold rounded
  system face ("py", "JS", "C++", "M↓", "TeX"; three letters set tighter,
  fitted to 16 points, centered on the letters), as Seti does. Every image
  is a template tinted by the image view's `contentTintColor`: one muted
  color per family (blue Python, C, TypeScript and Markdown; yellow
  JavaScript, JSON and keys; green shells, tables and patches; orange
  Rust, Swift, HTML, Makefile; and so on), at full strength on a dark
  tree and at 72% on a light one (`MCFileTypeTextIsLight` of the
  sidebar's text color decides, so a translucent sidebar follows its
  theme too); the yellow keeps 3:1 on near-white. Images and colors are
  made once per type and kept in vectors indexed by the type: a row
  only looks them up, and `configureTreeCell:` sets the image only when it
  changed. Each image's `accessibilityDescription` is the type's
  `typeName` ("python", "json", "dockerfile"), which the checks compare.
  Folders keep folder.fill (the GTK port gives special folders nothing
  of their own either), worktree rows their branch. Find in Folder's
  file rows use the same images and colors (they showed Finder's icon,
  `iconForFile:`, made per row); the tabs, Quick Open and the review page
  show no file icons. The `tree` suite checks a .py, package.json,
  Dockerfile, .png, README.md, an executable with no extension and the
  others by their image's description, that each is its type's one image
  in its type's color, and draws all 5,000 rows of a folder of twenty
  types page by page (`rowViewAtRow:makeIfNecessary:` and
  `cacheDisplayInRect:`), every row checked for its type's image, its
  CPU time bounded by 1.25 times that of 5,000 plain .txt rows drawn the
  same way in the same step (plus 250 ms): CI's runner took about 6 s of
  CPU for either, nearly all of it the rows and their names, not the
  icons (a tinted icon draws in about 0.06 ms on an M-series Mac, a
  drawn label no slower than a symbol).
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
  labels laid out by `TreeFocus::fit` in a `GtkCustomLayout`. Since item 107
  a window's record keeps its open folders (`FileTree::restoreView`).
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
  GTK port has it too** (2026-11-18, `linux/HANDOFF.md` item 98, the
  `tree_focus` suite's second half): `FileTree` keeps the set (`foci_`,
  `setFocusFolders`, `addFocus`, `removeFocus`, each through the core's
  `merged`, `added`, `removed`), and with several its model's top is a
  store of one `GFileInfo` per folder (`makeRootsModel`), labelled with
  its path in `onBind` (`minicode-tree-root`); `applyFocus` keeps the
  Mac's rules for what stays open (a root new to several opens, widening
  opens the old root), `showsPath` reveals through `TreeFocus::revealed`,
  `restoreFocus` takes the list through `restoredSet`, and `dropLostFocus`
  drops each gone folder with the Mac's three notes. The watches from the
  window's folder down cover every root's parents. The bar's label row is
  chips (a label and its ×) laid out in the breadcrumb's `GtkCustomLayout`:
  full paths when they fit, else `shortLabels` (switched from an idle,
  never during the layout), widths by `shared`; the + (`onAddFolder`) sits
  before the last ×. The window (`main.cpp`) has the menu bar's and
  palette's Add to Focus (`win.addtofocus`, the chooser when nothing new is
  left to add), the row's Add to Focus and Remove from Focus (hidden while
  they cannot act), and Add Folder to Focus… (`win.addfoldertofocus`),
  which is the system's folder chooser started in the window's folder (the
  GTK port has no palette folder picker), its answer held to that folder
  by `FolderPicker::inScope` (FolderPicker.cpp is in the Linux build for
  it). Persistence keeps the whole list. A drop below several roots is
  refused, as on the Mac. A window's record keeps the open folders inside
  and outside the roots (item 107).

- **The tree follows the active tab** (2026-11-18, the owner: "I have to
  hunt for the file that I am already using"; `EditorController+TreeReveal.mm`,
  its state in `EditorController+TreeRevealIvars.h`, the core's
  `TreeReveal` and `TreeFocus::shows` in FileDetails, `tree:auto-reveal`,
  the `tree_focus` suite's last part; `explorer.auto-reveal`, on by
  default): `activateTab:`, which every way to another tab goes through,
  calls `treeFollowActiveTab` where it called `reselectCurrentFileInTree`.
  It opens the folders down to the file with `treeOpenFoldersTo:widen:NO`
  (the walk `revealPath:open:from:widen:` was split into: each folder on
  the way listed with `loadChildren`, expanded, no reload), selects the row,
  and `scrollTreeRowIntoView:` scrolls only when the row is not wholly in
  view, then to the middle (`TreeReveal::scrollTop`, held to the rows'
  ends). `revealPath:` scrolls the same way now, so Quick Open and Reveal
  Active File center too. Left alone: a file outside every focused folder
  (`TreeFocus::shows`, the rule `treeFocusShowsPath:` keeps for opens; only
  a reveal widens), a diff or page, an untitled tab, a file gone or outside
  the window's folder, the row a click or Return in the tree is opening
  (`treeOpensPath:`, so a click on a row cut by the edge never scrolls),
  and a window's tabs coming back (`holdTreeFollowing:` around
  `restoreSession`'s activation, so the record's tree and scroll stand).
  While the tree is hidden (`treeOnShow`: the sidebar collapsed or at
  width 0, or Source Control, Agents or Processes in its place) the change
  waits in a `TreeReveal::Follower`; the four sidebar toggles and the
  sidebar's frame changes call `treeRevealIfShown`, which reveals it once.
  `applySettings` (`treeAutoRevealSettingsApplied`) reveals the file on show
  when the setting is turned on. The keyboard never moves: only
  `selectRowIndexes:` and the clip view's scroll. Trap: the tree following
  the active tab must not scroll for the row a click in the tree opens
  (`treeOpensPath:`), nor during a window's restore (`holdTreeFollowing:`),
  or a click on a row cut by the edge jumps the tree and a restored window
  loses its scroll. **The GTK port follows too** (2026-11-18,
  `linux/HANDOFF.md` item 107, `linux/src/TreeFollow.{h,cpp}`, the
  `tree_focus` suite's last part): `TreeFollow` holds the Mac's
  TreeReveal category's rules (the core's `TreeReveal::Follower`, the
  hold, the setting turned on) over three hooks from `main.cpp`
  (`treeFollowablePath`, `treeOnShow`, the setting); the editor's
  `onActiveChanged` calls `activeChanged` where it called
  `revealActiveInTree`. `FileTree::revealPath` centers: it selects the row
  without `gtk_list_view_scroll_to` (which only scrolls to the edge) and
  `centerRow` reads the row's place in the frame clock's after-paint and
  sets the scroll from `TreeReveal::scrollTop`; the tree's own reveals (a
  focus changed, hidden files shown, a root's label) keep the edge scroll
  (`reveal(path, widen, false)`, the Mac's `scrollRowToVisible` there).
  `FileTree::selectIfListed` is `reselectCurrentFileInTree`, and
  `opening()` is `treeOpensPath:` (set around the open callback in
  `actOnRow`). `showsPath` uses `TreeFocus::shows`. Hidden is the sidebar
  closed, another panel the sidebar stack's child, or the divider at 0;
  `layout->onChanged`, the stack's `notify::visible-child` and the
  divider's `notify::position` call `treeMaybeShown`. The hold is around
  `newWindow`'s and `openFolder`'s `attachHeldDocuments` to
  `showHeldActive`, and the window's record now keeps the tree's open
  folders, selected row and scroll (`openFolders`, `scrolled`), which
  `FileTree::restoreView` puts back once the listings have stopped
  coming. Quick Open reveals before it opens, whatever the setting, as the
  Mac's `openFileFromQuickOpen:`. One difference: GtkListView itself
  scrolls the row a click focuses wholly into view, so a click on a row
  cut by the edge nudges it in by the part cut off (never centered).
  Trap (GTK): a row's place is only right after the layout that holds the
  rows just put in, which comes after a frame's tick callbacks and may
  come after a low-priority idle, so `centerRow` reads it in the frame
  clock's after-paint, where no listing can have moved the rows since.
  Trap (GTK): a focus changed (Show Whole Folder, a breadcrumb, Add to
  Focus), hidden files shown or a window's record open the folders that
  were open from an idle as their rows come (`tryExpand`, from
  `onItemsChanged`), after the rows are in, so a check that closes one of
  those folders first waits for it to be open: closed before that idle,
  the close did nothing and the folder and its folders opened after it
  (`tree_focus`'s auto-reveal-off step failed so, 1 run in 5 beside
  `hints_panel`, 2026-11-19).
  Trap (GTK checks): the tree following the tab leaves open the folders
  earlier suites opened files from (Find in Folder's results open
  `replace_in_folder`'s rif4, which holds a.txt to d.txt), so a check finds
  a tree row by its path (`MCGuiTreeRow`), never by its name
  (`MCGuiFindLabel`): `tree_clicks` waited for its own folder's b.txt to
  go and found rif4/b.txt, which its folder closing moved up into view
  (FLAKY on CI in both runs from the one rif4 came in, 2026-11-19, passing
  alone; the folder had closed and stayed closed). Brought into view
  first (`MCGuiBringTreeRow`): `preview_tabs` waited for p-a.txt by name
  and failed in every soak round of 2026-11-19, the folders earlier
  suites had opened holding it far below the tree's last row (CI's order
  runs it sixth, the project nearly empty). Every suite now finds tree
  rows by path. A row that comes in at the top of a tree holding more
  rows than it shows stays above the pane, the rows on show kept where
  they were (GtkListView's anchor, `keepRows`): a check that wants the
  top row scrolls to the top on every look of its wait, never once
  before it (`tree_stall`'s a-tree-stall, in the tree's model and never
  on show for 30 s after eight other suites on a loaded machine,
  2026-11-19; agents' Worktrees node the same way).

- **A row going keeps the tree's place (GTK)** (2026-11-18, the owner: "If
  I delete a file in the filetree panel, rather than simply removing it
  from my current view, the view resets and I am brought to the top";
  `linux/HANDOFF.md` item 109, `./build/mutible-guitest tree_edits`). Two
  causes, both fixed. Move to Trash closed the file's tab, the tab before
  it came on show, and the tree followed it (`TreeFollow`, item 107; before
  it `revealActiveInTree`): to a file near the top, the top of the tree;
  `trashNow` also cleared the selection. Now `trashNow` calls
  `FileTree::removing(path)` (the row after the one going and the rows it
  holds is selected, else the one before) and closes the tabs inside
  `TreeFollow::quiet`, which neither reveals nor selects. And GtkListView
  itself (GTK 4.22) put the list at its top a frame or two after a row went
  that it was anchored on (the row last clicked or focused): its
  `update_adjustments` takes 0 for an anchor it cannot place. A selected
  file deleted on disk did it alone. `FileTree::keepRows`, from
  `onItemsChanged` (the tree model's first handler, so the list's row
  widgets and the selection are still as they were), notes the first row
  on show that stays and how far down it is drawn, and for 300 ms after a
  change that takes rows out `applyKeep` (the frame clock's "layout",
  connected after the window's, so before the paint, and `afterLayout`)
  sets a scroll gone to 0 back so that row is drawn there again. Only 0 is
  undone (GTK's shift for rows gone above is right; anything else is
  someone's scroll), and a press, a key, the wheel, a drag's edge scroll, a
  reveal's scroll or a window's record putting its own back end it
  (`stopKeeping`). The keyboard moves to the selected row only once the
  trashed row is out (`goingRow_`, `afterRowsGone`): moved before, the list
  anchors on that row where it is drawn and pushes the rows above the one
  going down by a row. A selected row that goes by any other hand (deleted
  or renamed on disk) passes the selection on the same way
  (`lostAt_`, `afterRowsGone`, only when its file is gone from the disk, so
  a folder closing or hidden files hidden choose nothing). Rename keeps the
  scroll too: the renamed row, revealed, is in view. Trap (GTK):
  GtkListView puts the list at its top a frame or two after the row it is
  anchored on goes, so `keepRows` undoes a scroll gone to 0 after rows go,
  and the keyboard moves to the next row only once the row is out; and the
  tabs Move to Trash closes must not be followed (`TreeFollow::quiet`), or
  the tree goes to the next tab's file. Trap (GTK checks, 2026-11-21): a
  check that scrolls the list itself (`gtk_list_view_scroll_to` to row 0)
  while a keep runs is undone like GTK's jump, since only the tree's own
  scrolls and the user's wheel and keys end a keep; `tree_clicks` after
  `session_browser`'s last step (files removed at the top level) never saw
  its folder (the soak). Bring a row on show with `FileTree::revealPath`,
  which ends the keep, and a suite that removes rows waits for
  `keeping()` to be false before it ends.
  A rename kept the tree's place in only about one run of three once
  other suites had run first (2026-11-20, `tree_edits`' "the tree kept
  its place" after `maximize`, `hints_panel` or a tree suite: the tree at
  908, was 736): the list went to its top as the name box closed, before
  any row changed, and the reveal of the renamed row then centered it
  from there. Trap (GTK): a popover on the list that has the keyboard as
  it closes gives it back only at the window's next after-paint, to the
  popover's parent, and when the idle that unparents the popover runs
  first GTK finds no parent and tabs into the window from its start, so
  the list focuses row 0 and scrolls to its top (GtkListBase's tab
  behavior "all", then `set_focus_child`'s `scroll_to_item`); the name
  box's "closed" gives the list the keyboard itself, before the idle (its
  focus row, the row asked about: no scroll).
- **Rows dragged out of the tree on GTK** (2026-11-19, linux/HANDOFF.md
  item 120, the Mac's #212): the row's GdkFileList (Move and Copy) dropped
  on the terminal types its path as the Mac's does (`Terminal::dropped`,
  TermDrop::paths, as a paste; VTE's GTK 4 build takes no drops), and on
  the editor's text opens the file in a tab of its own, a folder shown in
  the tree (`Editor::openDropped` through the link handler; a capture-phase
  drop target ahead of GtkTextView's, which typed the path). Checked by
  `tree_drags` with real XTest drags. The tree's name box (Rename, New
  File) keeps its typing through an app switch (`PopoverKeep.h`, checked
  by `edit_boxes`).

- **Where the listings come from** (2026-11-21, `MCTreeSource` in
  FileTree.h): a `FileItem`'s `source` is nil for this Mac's disk
  (`MCTreeList`, FSEvents) and, in a window over SSH, the window's
  `MCRemoteWorkspace`, which lists through the remote host and hears its
  notices in place of FSEvents (docs/notes/session-host.md, "The window's
  tree, Quick Open and Find in Folder over SSH"). Children take their
  folder's source. Code in the tree that looks at the disk itself
  (`fileExistsAtPath:`, `MCTreeStat`, the folder sizer, the Worktrees
  node, Quick Look, drags) must ask the source, or pass over a row that
  has one: `treeHasFileAt:` is the reveal's question for both. The tree's
  file operations other than New File say they do not work over SSH yet
  (`refuseOverSsh:`).

## Traps

Each trap's first sentence, in the order it appears above; the whole
text is in place in its section.

- The file tree (Mac): Trap: NSTableCellView moves its textField to the cell's width without a layout pass, so `MCTreeCell` places its fields again in `viewWillDraw`.
- The file tree (Mac): Trap: `NSTableViewLastColumnOnlyAutoresizingStyle` moves the column only by the change in the outline's frame, so a width lost once stayed lost and a reload cut every name off; `fitTreeColumn` fits it after each reload and frame change.
- The file tree (Mac): Trap: a within-app mask of Move alone had the terminal refuse every row (it takes only Copy, Generic or Link), and #187's check never saw it because its fake drag offered every operation; a drop check names the real source's mask.
- The file tree (Mac): Trap: the tree following the active tab must not scroll for the row a click in the tree opens (`treeOpensPath:`), nor during a window's restore (`holdTreeFollowing:`).
- The file tree (GTK): Trap: a row's place is only right after the layout that holds the rows just put in, which comes after a frame's tick callbacks and may come after a low-priority idle, so `centerRow` reads it in the frame clock's after-paint, where no listing can have moved the rows since.
- The file tree (GTK): Trap: a focus changed (Show Whole Folder, a breadcrumb, Add to Focus), hidden files shown or a window's record open the folders that were open from an idle as their rows come (`tryExpand`, from `onItemsChanged`), after the rows are in, so a check that closes one of those folders first waits for it to be open.
- The file tree (GTK): Trap (GTK checks): the tree following the tab leaves open the folders earlier suites opened files from (Find in Folder's results open `replace_in_folder`'s rif4, which holds a.txt to d.txt), so a check finds a tree row by its path (`MCGuiTreeRow`), never by its name (`MCGuiFindLabel`); a check that wants the top row scrolls to the top on every look, as a row coming in above the rows on show stays above the pane.
- The file tree (GTK): Trap: a name the icon theme lacks draws GTK's missing-image picture (no fallback by dropping parts of the name), so every name is checked with `gtk_icon_theme_has_icon` and each type's list ends with text-x-generic.
- The file tree (GTK): Trap: GtkListView puts the list at its top a frame or two after the row it is anchored on goes, so `keepRows` undoes a scroll gone to 0 after rows go, and the keyboard moves to the next row only once the row is out; and the tabs Move to Trash closes must not be followed (`TreeFollow::quiet`), or the tree goes to the next tab's file.
- The file tree (GTK): Trap (GTK checks, 2026-11-21): a check that scrolls the list itself (`gtk_list_view_scroll_to` to row 0) while a keep runs is undone like GTK's jump, since only the tree's own scrolls and the user's wheel and keys end a keep.
- The file tree (GTK): Trap: a popover on the list that has the keyboard as it closes gives it back only at the window's next after-paint, to the popover's parent, and when the idle that unparents the popover runs first GTK finds no parent and tabs into the window from its start, so the list focuses row 0 and scrolls to its top (GtkListBase's tab behavior "all", then `set_focus_child`'s `scroll_to_item`); the name box's "closed" gives the list the keyboard itself, before the idle (its focus row, the row asked about: no scroll).
- The file tree (GTK): Trap: the 300 ms that `keepRows` undoes a jump to the top for counted wall time, and on a loaded machine (several builds, load 50) the frame clock may draw no layout in 300 ms, so the jump came after the keep ended and `tree_edits` failed "the tree kept its place" about one run in nine; the keep lasts at least `kKeepFrames` layouts as well, and at most `kKeepMostUs`.
- The file tree (Mac): Trap: a remote window's rows are another machine's paths, so anything in the tree that reads this Mac's disk at a row's path (a stat, a file URL, Quick Look, the folder sizer) shows some other file or nothing; ask the row's `source` or pass over a row that has one.
