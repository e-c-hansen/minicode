# Git

Area notes moved out of `CLAUDE.md` (see its "Area notes" list). The text
below is as it stood there; section names quoted in it ("see ...") are
found through that list.

## Git panel (all three ports)

Shift+Cmd+G (the Mac's, outside a find bar; see "Find and replace") or
Ctrl+Shift+G swaps the tree for `MCGitPanel` (`replaceSubview:with:`, same
frame). All three panels draw from the core's `src/GitModel.{h,cpp}`
(`GitUi`: rows, the selection after a refresh, every argument vector, the
summary line, failure text, the graph's build, key, colors and tooltips, and
diff and commit text as styled runs in characters, which `MCGitStyledText`
maps to UTF-16; tested in `tests/gitmodel_tests.inc`, `git-ui:*`). It was
the GTK port's until 2026-11-06, when the Mac's `GitPanel.mm` still had its
own copy of every rule. The Mac passes its own date format (`DateFormat`,
the user's locale). A rule or wording changes there, once. The GTK port
(`linux/src/GitPanel`) and Android (`GitPanel.kt`, `git_jni.cpp`, git in
Termux through one bash loop) follow the same rules and wording (`linux/HANDOFF.md` item 10; `android/README.md`, "Source
control"). Logging: `G_MESSAGES_DEBUG=mutible-git` on Linux, `adb shell
setprop log.tag.MutibleGit DEBUG` on Android.

- **What runs**: status, diff, add, restore --staged, commit -m, rm --cached
  --force before the first commit, and for the graph for-each-ref, log,
  rev-list, show, rev-parse; `remote` with each status; on the Mac fetch, pull
  and push (below), Stage All (`add -A -- .`), Unstage All, `switch`,
  Discard's `restore --worktree` and the hunks' `apply` (below). Never a shell string: `MCGitRunSync`
  `posix_spawn`s an argument vector in the top level. Paths after `--`; add,
  restore and rm get `--literal-pathspecs` (`sp*ecial.txt` never stages
  `spXecial.txt`). Status `--porcelain=v2 --branch -z --untracked-files=all`;
  diffs `-c core.quotePath=false diff --no-color --no-ext-diff
  --submodule=short --src-prefix=a/ --dst-prefix=b/` (a user's noprefix,
  diff tool or diff.submodule would change the output),
  `--cached -M` for a staged rename, `--no-index -- /dev/null path` for
  untracked (status 1 means differs). `MCFindProgram(@"git")`, never
  /usr/bin/git's shim.
- **Environment**: `GIT_OPTIONAL_LOCKS=0` (a plain status rewrites .git/index,
  FSEvents fire, the panel refreshes: a loop), `GIT_TERMINAL_PROMPT=0`,
  `GIT_EDITOR=true`, stdin /dev/null, the wide PATH; stdout and stderr drained
  at once on their own queues.
- **Time limits** (`Git::timeoutFor`, one table for the Mac and GTK): 30 s for
  reads, 2 minutes for add, restore, rm, switch, stash, hash-object and
  update-index (filters, hooks, index.lock), 10 for commit (hooks, signing). git leads its own process group, which gets SIGTERM at the
  limit and SIGKILL 2 s later; the panel shows "git did not answer within 30
  seconds: <command on one line, 160 characters at most>" (`timeoutMessage`),
  or `heldOutputMessage` when git exited but something it started holds its
  pipes. git is reaped on its own thread; a timed-out for-each-ref skips the
  log. GTK: `runGitWithin` kills `-pid`, waiting with GSubprocess async calls
  on the git thread's own main context; that thread stays single on purpose (an
  add and its status must not pass).
- **Push, pull, autofetch** (Mac, 2026-11-06; plans in `GitUi::syncPlan`,
  tested in `git-ui:sync`): plain `pull` and `push` (the user's pull.rebase and
  push.default apply), `push --set-upstream <origin or the only remote>
  <branch>` without an upstream, `fetch <the upstream's remote>`; never forced,
  no confirmation (owner's git.confirmSync off). Limits: fetch 2 min, pull 5,
  push 10. `Git::talksToRemote` commands are spawned with `POSIX_SPAWN_SETSID`
  (instead of SETPGROUP; the group is the same), so ssh has no terminal to ask
  on and fails at once; `networkFailureText` adds what to set up. Each
  operation reads a fresh snapshot first, so the commands (`git.fetch` ...,
  View > Source Control) work with the panel hidden. `git.autofetch` /
  `git.autofetch-period` (`GitUi::autoFetchDue`): a 30 s timer in the panel,
  which is now made with the window (`ensureGitPanel`, EditorController+Git.mm;
  `stop` in windowWillClose:), fetches while the window is key, at most every
  180 s, per top level (process-wide map, main thread), never without a
  remote, backing off to an hour after failures. `autoFetchTickAt:` takes the
  time, so the GUI suite (`git_sync`) runs the waits without waiting.
- **Stage All, Discard, the file menu, the branch picker** (Mac, 2026-11-07;
  `GitUi::stageAllPlan`, `unstageAllPlan`, `discardPlan`, `branchItems`,
  `switchArgs`, tested in `git-ui:stage-all`, `discard`, `switch`; GUI
  `git_actions`). Like sync, each reads a fresh status, so View > Source
  Control works with the panel hidden (messages then also go to the status
  bar, `onNote`). **Discard always asks and is undoable**: the file goes to
  the Trash first (`trashItemAtURL:`; a failure discards nothing), then
  `restore --worktree` from the index, which is what the Changes row's diff
  is against (not `--source=HEAD`, which would also throw away staged work);
  a failed restore moves the copy back. Undo Discard (the panel's menu, the
  menu bar) trashes what is there now and moves the copy back, last 20.
  Refused: staged rows (as VS Code, unstage first), conflicts, `add -N`
  files. git.clean from the menu bar acts on the panel's selected row only
  while its list has the keyboard, else on the file on show. The picker is
  the palette with an `MCPalettePick` (a caller's list, PalettePanel.h);
  remote branches whose name a local branch has are left out, and a remote
  one is switched to with `switch -c <name> --track <remote>/<name>`. Tests
  swap `trashItemAtURL:` for a scratch folder, since the real Trash may be
  the user's own whatever HOME says.
- **Hunks and lines** (Mac, 2026-11-07; `GitUi::buildPatch`, `hunkPlan`,
  `diffTextWithActions`, tested in `git-ui:hunk-patch`, `line-patch`; GUI
  `git_hunks`). The panel's diff shows Stage Hunk and Discard Hunk (Unstage
  Hunk in a staged diff) as links after each hunk header, on the same line,
  so line i of the text stays line i of git's output (the menu maps a
  selection to lines by counting newlines). The panel is the diff view's
  delegate (links, and the hunk's and selected lines' items in its menu)
  and styles only the diff it showed last (`_shownDiff`, by NSData
  identity); commits get no delegate. The patch goes to `git apply
  [--cached] [--reverse] --whitespace=nowarn -` on stdin
  (`MCGitRunSyncWithInput`), built from the diff text with the applied-to
  side's numbers kept and the other's recomputed; a stale diff fails in git
  and is said so (`hunkFailureText`), then shown again. Discard Hunk asks,
  and first puts a copy of the file in the Trash (by way of a temporary
  folder, so the Trash names it as the file), which Undo Discard puts back.
  New, deleted, binary and conflicted files are staged whole.
- **Threads**: a serial queue per panel; results dropped if the root changed or
  a newer diff was asked. `refresh` coalesces 50 ms, only while in a window (on
  show, after actions, on window key, on the tree's FSEvents).
- **Errors**: git's stderr under Commit (6 lines, all in the tooltip); empty
  stderr uses stdout's last line (a commit with nothing staged explains itself
  there). Keys live in `MCGitTable keyDown:` and the message box
  (`MCCommitTextView`, Cmd+Return by keyCode 36/76); the diff takes the
  editor's slot (`showDiffNamed:`, `MCGitDiffText`), cut past 4 MB.
- **The graph**: `for-each-ref`; `-c log.showSignature=false log -z
  --topo-order --max-count=N HEAD @{upstream} --` (`--branches --remotes` for
  All branches; no `@{upstream}` when there is none, it is gone or HEAD is
  detached); `rev-list --left-right HEAD...@{upstream}` only when ahead or
  behind; a commit's `show ... -M --diff-merges=first-parent --stat --patch` (a
  merge's combined diff is usually empty). The status applies first, the graph
  after on the same queue, keyed on HEAD, branch, upstream, limit, checkbox and
  the for-each-ref output, so most refreshes run for-each-ref only. Show more
  adds 200. Lanes (`Git::layoutGraph`): a commit takes the leftmost lane
  waiting for it and ends the others waiting there; none waiting is a new tip
  at the right; the first parent continues the lane, others join a waiting lane
  or the first free one; free lanes close up, bending left in the same row. 12
  pt lanes, narrower past 40% of the panel.
- **Submodules, several rows, the verbs** (Mac, 2026-11-07): `Row::submodule`
  comes from status; `discardPlan` refuses a submodule (trashing it and
  restoring the gitlink left an empty folder, the checkout in the Trash) and
  the hunk planner refuses a gitlink diff, known by mode 160000 in its header,
  or with no index line at all (git writes none for a submodule at the same
  commit with changes inside) by its one "Subproject commit" line a side; a
  plain file holding such lines has a 100644 index line and stages by hunk.
  Staging a submodule still works. The change lists take several rows
  (`allowsMultipleSelection`; Command or Shift click shows no diff): Space,
  the row menu's Stage, Unstage and Discard act on all (`stageRowsArgs`,
  `discardBatchPlan`: one question, refused whole if any row would be, each
  file its own Undo Discard); they stay chosen across refreshes, by key, else
  by path. Commit and Amend, Undo Last Commit (`reset --soft HEAD~1`, or
  `update-ref -d HEAD` for a first commit; its message back in the box), Stash
  (`--include-untracked`) and Pop Stash are `GitUi` plans run by `runVerb:`;
  amend and undo ask when `headIsPushed`, undo asks for a merge.
- **The review view** (Mac, 2026-11-15, `ChangeReviewPage.{h,mm}`, core
  `ChangeReview.{h,cpp}`, `review:*`, `make guitest SUITES=git_review`):
  Zed's Uncommitted Changes as an `EditorPage` (key `review:<root>`, one
  per window). A file row's click, Return and Open Changes call the
  panel's `onReview` (Open as git Diff keeps the raw diff and its hunk
  links), a single click with `preview` (2026-11-18): the page goes in
  the group's preview tab (`showPage:preview:`, Model::Preview::User,
  the keyboard left in the list), a double-click on the same row or
  Return keeps it (`keepPreviewTab:`). `MCGitTable mouseDown:` hands
  every tracked press to `clickedRow:graph:clickCount:sameRow:modifiers:`
  (the GUI checks call it), which decides by `EditorTabs::treeClick`:
  the old `clickCount == 1` test opened nothing for a quick click on
  another row (the window server counts clicks by time alone) or a
  synthetic click, which the 1.6.3 walker met as "a click only
  selects". Command, Shift and Control clicks open nothing; arrows only
  move the selection, as in VS Code's list. A replaced preview tab's
  page is closed (`letGoOfReplacedTab:` calls `closePageOf:`); Review Changes (the panel's top, View > Source Control) opens
  it at its top. Staged files (HEAD to index, Unstage) then the Changes
  list (index to work tree, Stage and Discard), each file a header row
  then blocks split at every run of changed rows (`SideBySide::hunks`,
  never git's context), unchanged runs folded past 3 lines of context
  (`Opened`, keyed by the new line a gap starts on). Reading is one
  status, one `ls-files --stage`, one `ls-tree -r HEAD` and one `cat-file
  --batch` for every file (`ChangeReview::parse*`), then the disk; texts
  read as the gutter reads them (`QuickDiff::readOriginal` with the work
  tree's encoding; the staged list by the index blob's own mark), "\r"
  before "\n" ignored when aligning, and `ChangeReview::pick` writing
  lines from the other side with the base's line break, so a CRLF file
  stays CRLF. Stage and Unstage write the index text as Stage Change does
  (hash-object, update-index) after checking the entry's object; Discard
  asks through the panel, checks the file's bytes and the index are as
  read, puts a copy in the Trash (`+[MCGitPanel trashCopyOf:]`,
  `rememberDiscardAt:` for Undo Discard) and writes through
  `writeFileAtomically`, so a clean tab follows the disk ("Reload from
  Disk"); a dirty tab's file is refused. Every action, and every panel
  refresh (`MCGitPanelDidRefreshNotification`), reads again and keeps the
  place by `ChangeReview::anchorAt`/`rowFor` (the top row), except the
  first read showing an action's result (2026-11-18, `review:acted-place`):
  a press records `ChangeReview::actedPlace` (the file by key, the block's
  place on the side the action leaves alone, the other files' keys) and
  the row's height on screen, and the first read started after `act:`
  queued the work (reads carry a serial, `_readsStarted`) puts
  `rowAfterAction`'s row there: the next block of that file, else what
  followed it. The top row alone had kept another file's line in sight
  (the walker's jump from summary.py to models.py), and a file that went
  fell back to its index, which a staged list appearing above shifts. A
  refusal drops the place. **Pictures** (2026-11-18, `review:images`): a
  binary file with an image extension (`FileIcon::isImageExtension`; SVG
  is text) whose versions ImageIO reads (`CGImageSourceCopyPropertiesAtIndex`,
  no decode, on the queue) becomes `File::image` by `ChangeReview::showAsImages`
  (each side at most 64 megapixels, the 4 MB a side cap as before): the
  layout gives it `imageRows` empty `RowKind::Image` rows, and the page
  lays an `MCReviewImageBox` over them in the text view (a label and an
  NSImageView per version, at pixel size, at most 320 points tall, Before
  and After side by side), made again with each read and kept in sight on
  a sideways scroll. Commit pages and worktree pages read them the same
  way. The bytes stay in `Extra::oldImage`/`newImage`. The core's
  `ChangeReview::seeImages` (ChangeReviewRead.h, given an `ImageReading`:
  the GUI's header-only size reader and the row height; `review-read:images`)
  does the same in `readUncommitted` and `readWorktree`, which GTK reads
  with; the Mac keeps its own reading in ChangeReviewPage.mm. An action's note
  (`ChangeReview::ActionNote`, `noteRead:`) speaks of the files the action
  touched on the HEAD read on the queue as it ran (`act:`): it stays while
  the page shows those files as the first read shown after it did (or, for
  a refusal, as the page showed them then), and goes at any read that
  shows them otherwise or another HEAD. No serial numbers: reads and
  actions share the queue and reach the main thread in order, so every
  read shown after the note is its action's result or later (2026-11-17,
  two flakes on main: a commit landing before the first read, and a read
  asked for between the action and its note). Rows are one height
  (`ReviewRowHeight`), so places are arithmetic and layout is
  non-contiguous; tints, numbers and the buttons are drawn, the buttons
  pinned to the visible right edge (a sideways scroll redraws).
  **Accessibility** (2026-11-20, found by `make taskbench-mac`): the drawn
  buttons are `MCReviewButtonElement`s (NSAccessibilityElement, role
  button) named by `ChangeReview::buttonLabel` ("Stage change 1 in
  app.py", "Keep Current for conflict 2 in a.py", "Stage File app.py";
  `review:button-labels`), the tooltip as their help. `MCReviewTextView
  buttonElements` makes them for the rows on screen, kept by row and kind
  while the same read shows (a new read makes them again);
  `MCReviewScrollView` lists them after its own children, so they are
  siblings of the text area, not inside it (VoiceOver moves to them
  without interacting with the text). Their frame is the drawn button's,
  asked each time (it moves with a sideways scroll), and the text view's
  `accessibilityHitTest:` returns them. `accessibilityPerformPress` checks
  the button is still there and queues the press on the main queue:
  Discard asks, and a modal question inside the AX call would hold the
  caller until its timeout. **Trap: a read lands between an AX press and
  its action** (2026-11-20, git_review FLAKY twice on main, then red on
  #275): the queued press compared the page's data with the read it was
  pressed on and dropped itself, without a word, when any read had
  landed, and a panel refresh's read, often of nothing new, lands at
  any time (one queued while the suite's step ran). An element finds its
  button by what it acts on (`rowNow`: its own row while its read shows,
  else `ChangeReview::buttonRow`, the file by key and the block by its
  index, while that file's `fingerprintOf` is unchanged;
  `review:button-row`), so the press lands after a read of the same
  changes and beeps once they moved, never on another block that took
  the row; its frame follows the same way. `git_review` puts such a read
  on show (`showReadAgain`) between its press and the action. Commit and worktree pages get the same (Side
  by Side, Open). `git_review` and `worktree_review` find them through
  `MCGuiAXFind` (the harness's walk of the accessibility tree), and
  `git_review` stages a block by pressing one. **The graph's rows**
  (2026-11-20, found by `make taskbench-mac`, which read the commit off
  the screen): the table wraps each `MCGitGraphCell` in an AXRow and an
  AXCell stand-in that reads the view's `accessibilityLabel` and passes
  AXPress on to its `accessibilityPerformPress` (a scratch probe of a
  view-based NSTableView showed both), so the cell view is not an
  element itself. The label is `GitUi::graphRowLabel` ("Raise the limit,
  commit a1b2c3d, Ann, 2 hours ago, main (current), not pushed yet", the
  subject first; "Show 200 more commits"; `git-ui:graph`); a press queues
  the commit's full hash and `pressedGraphCommit:` finds it in the graph
  on show then (a read may have moved the rows, as on the review page),
  selects it and opens it as a click does (the preview tab), beeping when
  it is gone; "Show more" loads more. `git_panel` reads two rows' labels
  and opens a commit by pressing its cell. New,
  deleted, binary, submodule, conflicted and `add -N` files go whole
  (`wholeOnlyReason`). **GTK** (2026-11-19, `linux/HANDOFF.md` item 118,
  GUI `git_panel` and `git_review`) has the same over the same core: a file
  row's single click opens the page at that file in the preview tab
  (`GitPanel::clickedRow` by `EditorTabs::treeClick`, with `ClickCount`;
  `Editor::showPage(page, preview)`, whose replaced tab takes its page with
  it), the keyboard left in the list; a double-click, Return and Open
  Changes keep it and give the page the keyboard; Open as git Diff shows
  the raw diff with its hunk links; Ctrl and Shift clicks and the arrows
  open nothing. The acted place is kept by `actedPlace`/`rowAfterAction`
  with a read serial (`readsStarted_`, `actionPosted()` when the work goes
  to the git thread). **Trap**: a scroll to an iter just after the page's
  text is replaced uses line heights GTK has not measured, and the page
  went to its top (the top-row anchor did so too); `scrollRowTo` scrolls
  to a mark, then a tick callback (`onSettle`) puts the row right each
  frame until it holds three, and the reveal, the acted place and the top
  row all go through it. Pictures are sized by
  GTK's image loaders from their headers and decoded at the size drawn,
  on the git thread, and stand in a child anchor on the file's first
  `RowKind::Image` row, the others tagged invisible. Not on GTK: the commit
  page (a graph click shows `git show` in the diff tab) and animated GIFs
  (the first frame). Android can follow over the same core. **A commit from the graph** opens the same page in commit
  mode (`initForCommitsWithRoot:`, key `commit:<root>:<hash>` since
  2026-11-18, through the panel's `onReviewCommit`): a single click shows
  it in the preview tab, a commit page already there taking the new commit
  (`showCommit:` again), a double-click or Return keeps it, so kept commit
  pages stay beside the preview one; Line History's click opens in preview: `show -s` for
  the header (`Git::parseShow`), `diff-tree -r -z -M` against the first
  parent (`--root` for a first commit) for the files
  (`ChangeReview::parseRawDiff`), their blobs in one `cat-file --batch`,
  all `Section::Plain`, so no buttons but Side by Side. After a commit the
  line under the button is `GitUi::commitDone` ("Committed “x” to main.",
  the short hash muted after it; the GTK and Android panels show
  `commitDoneText`). The line over the graph (`summaryText`) wraps, its
  height measured on every layout, with the whole of it as its tooltip.
  **The page's note** (2026-11-16, G1) is the last action's words, at the
  bar's right, or on a line of its own under the bar when they do not fit
  or carry a button; it goes once a read shows the files it speaks of
  otherwise (`ChangeReview::fingerprintOf`, see above), so "Staged a
  change" never outlives a commit, an unstage or a discard. The
  header counts files (`ChangeReview::fileCount`), not list entries.
  **Apply to Main** (worktree mode; A1b) puts its result on that line
  with Show in Source Control (`onShowSourceControl`: the window's
  `showSourceControlChanges` when it is on the main checkout, else a
  window on it), and the button reads "Applied to Main", disabled, until
  a read shows the worktree's changes other than those applied.
  **A worktree's page** (Compare with Main, 2026-11-15, `make guitest
  SUITES=worktree_review`): `diff --raw -z -M <merge base>` in the
  worktree (its new side read from disk) plus `ls-files --others
  --exclude-standard`, `Section::Plain` with Side by Side and Open. Apply
  to Main… (`GitUi::applyToMainPlan`) reads main's status first and
  refuses, naming them, when main has changes of its own in those files,
  else asks naming the files. After the yes, both ports run the core's
  `ChangeReview::applyToMain` (the Mac through a `GitRunner` over
  `MCGitRunSync`, since 2026-11-19): the worktree is read again
  (`worktreeRawArgs` and `untrackedArgs`, `ChangeReview::applyNow`) and
  anything it lists that the question did not (a file changed or made
  since the page's read, either side of a rename) refuses the whole
  (`GitUi::applyStaleText`, naming them); main's status is checked again
  (a change of the user's own made while the question was up refuses
  too); the patch is the worktree's `diff --binary` against the merge
  base held to the files asked about (`worktreeDiffArgs(base, paths)`,
  `--literal-pathspecs`), plus a `diff --no-index /dev/null <file>` per
  file asked about that is untracked now (never `add -N` in the
  worktree's index), given to `git apply` in the main checkout (the
  first `worktree list --porcelain` entry), all or nothing; when that
  fails, `apply --3way`, which leaves conflict marks (and git's unmerged
  entries) in the files that clash and stages the rest, which are then
  unstaged (`GitUi::unstageArgs`, a rename's source path with its new
  one), so everything arrives unstaged. Never a commit, merge or branch
  switch. `apply` has git's two-minute limit (`Git::timeoutFor`). Checks:
  `review-worktree:git`, `:apply-checked`, `:apply-rename`, and the
  worktree_review suite's late.txt.
- **Change bars** (Mac, `GitGutter.mm`, core `QuickDiff`): the original is
  the index entry, as VS Code's quick diff (`ls-files --stage`, then `cat-file
  blob` of its object), read again only when the file or the index file's stat
  changes; diffed off the main thread 0.3 s after typing pauses (one timer, not
  one a key), with `LineDiff::diff(..., joinBlankGaps = false)`; dropped when
  an edit came meanwhile. Lines are the editor's (a lone '\r' and U+2029 end
  one; CRLF against LF is no change). Revert Change is a buffer edit through
  `replaceRanges:withStrings:actionName:`; Stage Change is VS Code's
  `hash-object -w --stdin --path` and `update-index --cacheinfo`, refused when
  the entry no longer names the object the bars came from. The panel posts
  `MCGitPanelDidRefreshNotification` so the bars look again, and the gutter
  watches the repository folder itself (a vnode source, `MCGitDirWatch`;
  git renames index.lock and HEAD.lock into it), so git in a terminal with
  the panel hidden moves them at once. A tab that keeps its file's UTF-8 BOM
  apart (`MCTab bom`) diffs against the index text without its U+FEFF
  (`QuickDiff::stripBom`), and Stage Change puts it back (`restoreBom`):
  compared whole, line 0 always read as changed and Revert wrote a second
  BOM. For a UTF-16 tab (`MCTab encoding`) the blob is UTF-16 too:
  `QuickDiff::readOriginal` reads it unit for unit without its mark and
  `stagedBlob` writes Stage Change's text back so. Option+F5 and
  Shift+Option+F5 (`nextChange:`) are Editor commands the key monitor runs.
- **Merge conflicts** (Mac, 2026-11-16, the owner's G2; core
  `MergeConflicts.{h,cpp}`, `conflicts:*`; `EditorConflicts.{h,mm}`;
  `make guitest SUITES=git_conflicts`, a real diff3 merge): VS Code's and
  Zed's choices over each `<<<<<<<` block. The core reads the blocks (a
  marker is seven characters at a line's start then the end or a blank and
  a label; eight or more, or indented, is text; a block without its
  separator or end is left alone, a second `<<<<<<<` starts again; CRLF as
  LF) and gives each choice as one splice. In the editor `CodeTextView`
  owns an `MCEditorConflicts` (`tv.conflicts`): tints behind the text
  (`editor.merge-current`, `editor.merge-incoming`, VS Code's
  merge.*ContentBackground, markers' lines at 2.5 times the alpha, the
  diff3 ancestor gray), Keep Current, Keep Incoming and Keep Both drawn
  over the text after the `<<<<<<<` line's label (pinned left of the
  visible right edge when that is too far), then "Conflict 2 of 3"; a press
  is taken first in `mouseDown:` and is one `replaceRanges:` step ("Keep
  Current Change"), the status bar saying how many are left
  (`resolvedNote`). Blocks follow edits at once (`followEdit`: an edit
  touching one, or ending at its start, drops it) and are parsed again 0.2
  s after a pause, at once when the whole text is replaced or editable
  flips; only while `wanted` (the change bars' `gitGutterPath`: source text,
  not a preview, a diff or large-file mode). The change bars skip a
  conflicted file, so Next and Previous Change step between conflicts when
  a file has them. On the review page an unmerged file in Changes is read
  from the work tree and laid out by `ChangeReview::conflictFile`: one row
  per line, each block a `RowKind::Conflict` run with its `ConflictPart`
  (markers kept), one number column, Keep buttons on its first row and in
  the right click, Side by Side showing every current side against every
  incoming one (`resolvedAll`); a choice writes the file through the
  atomic writer after checking its bytes are as read (no Trash copy; a
  dirty tab refuses), and with none left (`canMarkResolved`) the header's
  Mark Resolved runs the panel's stage arguments without the conflict
  question. A binary, missing or too-large conflict keeps the old note.
  The GTK port has the same (`linux/HANDOFF.md` item 95,
  `linux/src/EditorConflicts.{h,cpp}`, `git_conflicts`), the review page's
  right click aside.
- **Line history** (Mac, 2026-11-16, A7, `EditorLineHistory.{h,mm}`,
  `EditorController+LineHistory.mm`, core `LineHistory.{h,cpp}`,
  `linehistory:*`, `make guitest SUITES=line_history`): blame that knows
  Claude Code's sessions. Each line is a commit (`git blame --porcelain
  --contents - -- <name>` with the buffer on stdin, through
  `MCGitRunSyncWithInput` and its 30 s limit, so lines are the buffer's),
  a session's uncommitted edit, or "Not committed"; a file git does not
  track (or no repository) is all uncommitted, a git that timed out or
  failed otherwise is Unknown (no words) with the reason in the box.
  `LineHistory::linesWritten` walks one session's `AgentEdits::collect`
  changes last first: each new text must be found exactly once in the
  text as it was just after the change (the file now with later changes
  undone), the region widened to whole lines (and past a put-back text
  that would join the next line), and only lines a `LineDiff` of the
  region before and after says changed are credited, so an Edit's quoted
  context is not; a created Write takes every line still mapped to now; a
  rewrite of an existing file, a removal, a notebook cell or a text not
  found once ends the walk, which restarts once from the file now for the
  earlier changes. Only uncommitted lines can be a session's; of several
  sessions the latest change wins (`combine`). `AgentEdits::Change` now
  carries `prompt` (the last prompt before the call), and the box shows
  its first line (`promptLine`). The Mac: the gutter's `onHoverLine` (line
  numbers, dots and change bars, not the test or fold lanes) shows a
  child panel after 0.5 s, following the pointer down the gutter, a click
  opening `showCommitReview:title:` or `MCAgentWindow
  showChangesToFile:`; View > Show Line History
  (`git.blame.toggleEditorDecoration`, no key; `MCLineHistoryInline` in
  the app defaults, every window) draws the words after each line's last
  fragment through CodeTextView's `drawOverText`, clickable through
  `onMouseDownFirst`, the box on rest through `onPointerMoved`. Looks run
  on a serial queue only while wanted (the words on, or a box asked for),
  0.6 s after typing stops; an edit hides lines from its offset on
  (`_validBefore`) until the next look; each path's last answer is kept
  with its text (8 files) and reused on a tab switch while the git panel
  and the Agents model have not published since. Not done: GTK, "committed
  from session X" (left out until it can be exact), and a gap between
  git's and LineDiff's alignment of blank lines around an insertion (one
  may read "Not committed" or the commit where the other reads the session).
- **The status bar's branch** (Mac, 2026-11-19, round 9's "no branch and
  no git button"; `StatusBranchItem.{h,mm}`, core `Git::statusBranch`,
  `git:status-branch`; GUI `git_panel`): VS Code's bottom left, at the
  status bar's left end before "Saved ...": SF Symbols'
  `arrow.triangle.branch`, the branch (HEAD's short hash when detached)
  with "*" for any status entry (untracked too), then "↑2 ↓1" only when
  `# branch.ab` came (an upstream git compares with; "↑0 ↓0" when level,
  so there is always something to click to sync). The name runs
  `gitCheckout:` (the palette's branch picker), the counts `gitSync:`
  (the panel's runSync, which asks nothing, as the button); one tooltip
  on both names the two clicks. **No git of its own**: `Git::decorationArgs`
  gained `--branch`, so the tree's colors' one status per change on disk
  (`MCTreeGitColors`, which already looks after writes to .git's index,
  HEAD and refs, and skips objects and logs) carries the headers, and
  `onBranchChange` fires when `statusBranch` differs. Hidden when the
  status fails or the folder leaves a repository (`setRoot:` clears it).
  A root below the top level sees no .git events, so a commit made in a
  terminal there shows at the next change under the root, as the tree's
  colors do. At most a third of the bar; the name truncates, the counts
  never. The GTK port can show the same `StatusBranch`. **Header trap**:
  `StatusBranchItem.h` takes NSStrings, not `Git::StatusBranch`, because
  `EditorController+Private.h` imports it and every GUI suite imports
  that; a suite's own `Git(c)` helper then clashed with `namespace Git`
  ("reference to 'Git' is ambiguous", CI's first run).
- **The session's page in a browser** (R3's fourth slice, 2026-11-19;
  session-host.md, "Source Control and the review in the page"): the
  page lists the uncommitted changes and shows their review from the
  core's `readUncommitted`, read only, held to the folder, git run with
  the core's arguments (the Mac's through `MCGitRunSync`); the window's
  FSEvents say when to read again. Since R3's last slice it also shows
  the window's open commit pages (the core's `ChangeReview::readCommit`,
  the Mac's `ReadCommit` ported, `review-read:commit`) and worktree pages
  (`readWorktree`, which now marks the work tree's files `workRead`), and
  every review's pictures (session-host.md, "The window's commit and
  worktree pages, and pictures"). The Mac's own pages still read with
  ChangeReviewPage.mm's copies.
- **Tool guard trap**: in a worktree-isolated agent session, git commands in
  compound or unusual forms are refused (even a filename containing "git"); use
  plain `cd x && git ...` lines or a script file.

## Traps

Each trap's first sentence, in the order it appears above; the whole
text is in place in its section.

- Git panel (all three ports): **Header trap**: `StatusBranchItem.h` takes NSStrings, not `Git::StatusBranch`, because `EditorController+Private.h` imports it and every GUI suite imports that.
- Git panel (all three ports): **Tool guard trap**: in a worktree-isolated agent session, git commands in compound or unusual forms are refused (even a filename containing "git"); use plain `cd x && git ...` lines or a script file.
- Git panel (all three ports): **Trap**: a scroll to an iter just after the GTK review page's text is replaced uses line heights GTK has not measured, and the page went to its top (the top-row anchor did so too); `scrollRowTo` scrolls to a mark, then a tick callback (`onSettle`) puts the row right each frame until it holds three, and the reveal, the acted place and the top row all go through it.
- Git panel (all three ports): **Trap: a read lands between an AX press and its action** (2026-11-20, git_review FLAKY twice on main, then red on #275): the queued press compared the page's data with the read it was pressed on and dropped itself, without a word, when any read had landed, and a panel refresh's read, often of nothing new, lands at any time (one queued while the suite's step ran).
- Git panel (all three ports): **Apply to Main checks and applies the same files** (round 10's defect 6, 2026-11-19): the check ran on the page's read and the apply on a fresh `git diff <base>` with no pathspec, so a file the agent changed after the read was patched into main over the user's own edit, unchecked, and an untracked file made since was dropped; the apply reads the worktree again and refuses anything new, checks main again, and holds the diff to the paths asked about. And after `apply --3way` a rename is one status entry: restoring only its new path left the source's deletion staged; unstage `origPath` too.
- Git panel (all three ports): **A refusal's note settles on the read after it** (2026-11-19, main's CI after #245): a worktree page settled every note on the read on show, but a refused Apply to Main asks the user to look over the read that follows (it shows the file changed since), so that read took the note away; `_noteSettlesOnNextRead` settles a refusal's note on the next read. In a check, wait for the read that shows the change (its row), never for any later read: one started before the write can land first.
