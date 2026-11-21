# Find and replace

Area notes moved out of `CLAUDE.md` (see its "Area notes" list). The text
below is as it stood there; section names quoted in it ("see ...") are
found through that list.

## Find and replace (Mac)

- `TextFind` matches a line at a time: plain text with FolderSearch's
  one-to-one folding, or a regex (ECMAScript, std::regex deciding what is
  valid) over UTF-32 code points. Whole word is VS Code's edge check;
  replacements take `$1`..`$99`, `$&`, `$0`, `$$`, `\n`, `\t`, `\\` for a
  regex. **Regexes run on `Regex.{h,cpp}`** (2026-11-07), a Pike VM whose
  time is linear in the line, with JavaScript's matches (checked against
  node on a million random cases, and against std::regex in `regex:oracle`);
  a pattern it refuses (backreferences, lookaround, `[[:alpha:]]`, a lone
  `]`) goes to std::regex on a worker thread of the query's, left behind
  after 2 s (`Query::tooSlow`, `FolderSearchResult::tooSlow`). On a line
  over 2,000 code points it is tried a start at a time and reads at most
  8,192 past each (`setRegexReach`): libstdc++ 15 recurses per character
  (GCC bug 86164) and a 50,000-character line overflowed the worker's
  256 MB stack under TSan, a crash that would end the app. Traps:
  libc++ and libstdc++ each break JavaScript's repetition rules in places
  (groups unset per repetition, empty repetitions not counting), libc++
  finds no `\b` at a line's end once a search starts past 0, and libc++
  throws error_complexity (a silent "no match") on short exponential
  patterns rather than hanging, while libstdc++ hangs; std::regex's own
  traps: libc++ lets `^` match where any search starts, `match_not_bol`
  being dropped beside `match_prev_avail` (only its private
  `__no_update_pos` stops it), so a search from inside a line runs the
  pattern with each anchor `^` made `(?:\b\B)` (`Fallback::inside`,
  `find:regex-backtracking-anchors`); libstdc++ tests bracket ranges through the C locale's
  ctype, so `[а-я]` ignoring case missed Ж on CI's Linux job, and a
  case-insensitive regex runs over the line folded to lower case, one code
  point for one (the linear engine folds the same way). Preserve case is VS
  Code's buildReplaceStringWithCasePreserved, for replacements with no
  groups.
- `MCEditorFind` replaced NSTextFinder (`usesFindBar = NO`). Since 2026-11-06
  it is laid out from VS Code's findWidget.ts and CSS, measure for measure
  (the owner asked for VS Code's look; the toggles had sat far from the
  query): 419 wide, the replace chevron down the left edge, the toggles
  inside the query, then the count, arrows, Find in Selection, close; the
  reduced/narrow/collapsed widths; a sash on the left edge. The find_bar
  suite checks the frames against the CSS. Cmd+F seeds as VS Code's default
  ("always": one-line selection, else the word at the caret, never several
  lines). Find in Selection's VS Code key, Option+Cmd+L, is Toggle Task's in
  the editor, so the bar takes it only while its fields have the keyboard
  and the menu item has no key. Trap: CALayer copies `shadowPath`, so
  release the path you made, never the one the getter returns (that crashed
  in the next CA commit). It searches as you type on a serial queue (cancelled between
  lines, applied `whenCurrent:`), over a copy of the text made once per
  edit of it (`MCFindText`: an NSString copy on main, UTF-16 on the
  queue), never once per keystroke in the query, and draws through `drawBehindText`. Cmd+G /
  Shift+Cmd+G, Return / Shift+Return (Shift+Cmd+G is Toggle Source
  Control's menu key since 2026-11-16, the owner's call: Find Previous has
  no registry key, and the bar, the terminal's and browser's
  `MCTerminalFindBar`, and Find in Folder's window, whose
  `MCSearchContent` steps through the results with Cmd+G and Shift+Cmd+G,
  take it in `performKeyEquivalent:` before the menu); Option+Cmd+C, W, R; Return in the replace
  field replaces and moves on; Shift+Cmd+1 replaces only while the bar has the
  keyboard (Go to Tab 1 elsewhere); Up and Down in either field walk its
  history (2026-11-14, `FindHistory.h`, VS Code's: newest last, a text
  kept once, 50, added after half a second or on Return, every window's
  bar sharing it); Option+Cmd+Return replaces all as one
  programmatic edit; Esc; Cmd+E. A preview finds but does not replace. Menu
  items still send `performTextFinderAction:` with NSTextFinderAction tags, so
  the terminal and diff view answer their own.
- **The find bar in a large file** (2026-11-19, round 9's C3 on 1.6.5: a
  259 MB log's last line took about 2 s, VS Code's and Zed's well under
  one, and Cmd+F's seeded "2026" stayed marked on every line while the next
  query ran). A text of 1M units or more (`kStagedUnits`) is searched in
  stages on the queue through `TextScan` (core, `TextScan.{h,cpp}`): the
  needle every match holds (`TextFind::requiredText`; ignoring case its
  longest ASCII run, since only ASCII folds to ASCII, which
  `textscan:needle-ascii-case` checks over the BMP) is looked for sixteen
  bytes at a time (GCC/Clang vector extensions, the first and last units
  both present, a letter matched by setting 0x20), and only a line holding
  it goes through TextFind; plain text whose needle is the whole query
  (matching case, or ASCII) needs no line matched. The matches are
  TextFind::findAll's exactly (`textscan:same-as-findall`: UTF-8 bytes,
  ASCII bytes, UTF-16 read in blocks smaller than lines, ranges, first and
  last). Find in Folder's needle is the same class now. Stages, each sent
  to main as it is ready (`MCStagedNews`, merged into `_matches`): the
  lines on screen (`visibleCharacters`, no layout); the first match from
  the anchor, wrapping, selected at once with ±16K units of marks
  ("1 of …" when it was found searching from the top, the anchor there or
  wrapped to it, so it is the first of all; else "? of …" until the
  matches before it are counted); then
  every match counted from the top, kMatchLimit kept for the marks, parts
  sent every 100 ms; the count shown is exact ("1 of 3000000"), the tooltip
  saying only 20,000 are highlighted. Find Next while counting, or past the
  kept matches, searches the text from the selection on its own queue
  (`_navQueue`, `findByScanning:from:`, `MCFirstIn`/`MCLastIn`), so it
  never waits for the count; Return typed before the first match came runs
  after it (`_afterFirst`). The text read is the file's own ASCII bytes
  while the piece string is unedited (`MCHeldAsciiBytes`, PieceString.h,
  set by `MCReadDiskText`), else its UTF-16 in 1M-unit blocks. Timings on a
  246 MB, 3,000,001-line log (Apple Silicon, -O2): NEEDLE-4242 on the last
  line 1,190 to 1,860 ms through TextFind's blocks before, 19 ms from the
  bytes (113 ms from UTF-16 blocks, an edited text); counting 3,000,000
  matches of "request" 73 ms (141 ms). **Marks never outlive their query**:
  drawing reads `_shownGen`/`_shownVersion` (what `_matches` are for, in
  part while counting), and a staged search empties them and redraws at
  once; a small text keeps its old marks until its results come a few ms
  later, so typing never blinks them. **The seed** follows VS Code's
  `editor.find.seedSearchStringFromSelection` "always" (a one-line
  selection, else the word at the caret; VS Code's other values are "never"
  and "selection", which seeds only from a selection), but in a text of
  20M units or more (`LargeFile::kSizeThreshold`) a word at the caret is put
  in the field and not searched (`seedWithoutSearching:`, `seedPending`,
  an empty count with a tooltip) until a key in the field, an option, or
  Return, which goes to its next occurrence as if the search had run when
  the bar opened; a selection is the user's choice and is searched. The
  `large_file` suite checks a 100 MB log: the seed waits, Return lands on
  line 2's "2026", a new query leaves no mark on screen, the needle's first
  match in `GCHECK_TIME(firstMatchCpuMs, MAX(150, 8 × a memchr pass))` of
  the queue's CPU, "1 of 1", and a match on every line counted with under
  50 ms of main-thread CPU between two fires of a 2 ms timer.
- Find in Folder's window is VS Code's search view (searchview.css): query
  and replace with their toggles inside, Replace All beside, files to
  include/exclude (`FolderSearch::PathFilter`, VS Code's queryBuilder rules)
  behind "...", and an NSOutlineView of files (count badges) over lines (the
  match highlighted, or struck through beside its replacement). **Each
  line's row is its excerpt** (2026-11-18, C3: a bare line each): its
  number in a gutter (as wide as the file's largest number, so they line
  up) and `kContextLines` (1) above and below, muted, with theirs, from
  `FolderSearchOptions::contextLines` (`FolderSearchMatch::before` and
  `after`, display text as the line's; the core keeps them for the rows
  found only, crosses a large file's pieces both ways, `search:context`
  comparing them with a whole read, and `dropRepeatedContext` leaves out a
  line a neighbouring row of the file shows, so consecutive hits read as
  one excerpt); the row is `4 + 18 × lines` high
  (`heightOfRowByItem:`), `MCSearchCell` flipped with the actions beside
  the line itself, and a click or Return opens the hit's line. The TODO
  list asks for none (numbers only). GTK and Android ask for none yet.
  `hits`
  stays the flat list of lines that tests and `replaceMatchAtRow:` index.
  Find in Folder on a subfolder puts `./sub` in files to include.
- Find in Folder uses `FolderSearch::search` (two characters, one for an
  untrimmed regex; `.git`, `node_modules`, `build`, the venvs and caches and
  what the folder's `.gitignore`/`.ignore` files ignore (from the window's
  folder down, `search.use-ignore-files`) skipped, dotfiles searched; a
  NUL in the first 8 KB marks a file binary, left out unnamed; text that
  is not UTF-8 is read as Windows-1252 (`decodeWindows1252`, 2026-11-18,
  C3: a Latin-1 log had been left out and named; a NUL anywhere in such a
  file still makes it binary; a large file switches at its first piece
  that is not UTF-8, keeping the matches before), and Replace in Folder,
  which writes UTF-8 and UTF-16 only, leaves such a file alone and names
  it when it holds a match (`FolderReplacePlan::otherEncoding`; Replace
  Match on one of its rows says why); clicking its row opens it as text
  at the hit's line (C3b, 2026-11-18: it had opened in Quick Look; the
  editor reads it by the same rule, `readsAsWindows1252`, see
  sessions.md); the files left out counted and named (`skipped`,
  shown under the results, one by name);
  a file over 1 MB (`maxFileSize`) read and matched in 4 MB pieces cut at
  line breaks (`Matcher::stream`, 2026-11-16, C3: an 82 MB log had been
  skipped), the same rows as a whole read (`search:stream` compares them at
  odd piece sizes), left out only for a line over 64 MB; Replace in Folder
  still reads whole and leaves such a file alone, naming those with a match
  (`FolderReplacePlan::tooLarge`) in its question and status; UTF-16
  with a byte order mark decoded as ripgrep does (`decodeUtf16`; its NULs
  had hidden it), Replace in Folder writing it back in its own encoding
  (`readTextFile` with an encoding, `encodeText`) and leaving out one with a
  lone surrogate; 2000
  lines by default, 20,000 on the Mac; ANSI stripped; symlink loops and
  named pipes safe; generation-counted and cancelled per file). Since
  2026-11-07 the walk is parallel: folders are listed and files read and
  matched on up to 8 threads (the calling one too), earliest in the serial
  walk's order first (a priority queue keyed by each entry's place), and the
  calling thread merges in that order, so results, the limit and counts are
  the serial walk's (`search:parallel-order` compares them on 1, 2 and 8
  threads). A folder reached through a symlink is listed only when the
  merge reaches it, so a loop is never followed. Lines are matched only
  where the file holds the query, or the literal a linear regex requires
  (`TextFind::requiredText`), found in the whole buffer first. About
  ripgrep's time on a 7,600-file tree (180 to 220 ms against 165 to 215 for
  "return"; 250 to 300 against 200 to 220 for a regex). Replace in Folder
  (Option+Shift+Cmd+F) counts and asks; a file in a clean tab here is edited in
  the tab through the rename applier (`applyEditsToPaths:`); a file with
  unsaved text in any window is left alone and named; others go through
  `FolderSearch::writeFileAtomically` (mode kept, synced, renamed; symlinks
  followed; hard links written in place). A file under two names (a symlink,
  a hard link, a symlinked folder) is counted and replaced once, by device and
  inode, and one reached through a link to outside the folder is named in the
  question (`FolderReplaceFile::outside`). Replace Match replaces only while
  the row's line still reads as searched. `replaceOneAt` maps a result's byte
  column to a tab's UTF-16.
- **For the session's browser page** (2026-11-21, docs/notes/session-host.md,
  "Quick Open and Find in Folder in the page"): the host runs
  `FolderSearch::search` on a worker with the Mac's options and three
  more, which the app's windows leave off: `skipDotfiles` (the page's
  tree shows no dotfiles unless Show Hidden Files is on), `confine` (a
  symlink whose real place is outside the folder is neither read nor
  entered, `insideRoot`), and `onRows`, called on the merging thread with
  each file's rows as they are taken (repeated context dropped between
  them), so the rows go to the page as they are found.
- **Where Option+Shift+Cmd+F puts the keyboard** (2026-11-20, found by
  `make taskbench-mac`: with nothing searched yet it went to the replace
  field, so the search typed first became the replacement): as VS Code's
  `findOrReplaceInFiles`, `openReplaceInFolder:` shows the replace row and
  gives the replace field the keyboard only when `seedSearchFromSelection`
  just made the editor's one-line selection the query
  (`showReplaceFocusingReplace:`), else the query, its text selected (an
  NSTextField selects all as it takes the keyboard), whether or not it
  holds an earlier search. Tab goes on to the replace field (the window's
  key loop). Opening a result (double-click, Return) leaves the keyboard
  in the editor's text (`jumpToLine:`), though the tree had it before.
  `replace_in_folder` checks all three cases, `search` the result's open.
- **Find in Folder's keys and dismissed rows** (2026-11-18, after a round 9
  walker on 1.6.3). Return in any of the window's fields searches, the
  replace field's too (VS Code's searchWidget submits the search there);
  Replace All is the button or Option+Cmd+Return, taken by
  `MCSearchContent performKeyEquivalent:` (`onReplaceAll`, only while the
  replace row shows, else a beep) because the Edit menu's Replace All
  (`performTextFinderAction:`) is validated by the field editor first and
  was off in this window. Both go through `replaceAll:`, so the question is
  the same; before `runModal` it drops key events already queued
  (`discardEventsMatchingMask:`), so keys typed while the matches were
  counted never answer it. A row's x (and the right click menu's Dismiss)
  records the row in `FolderReplaceExclusions` (core, `FolderSearch.h`):
  a file whole, or a line by its number and its row's text. Replace All runs
  `planReplace` and then `excludeDismissed`, and `replaceInPaths:` leaves
  those lines alone (`dismissedLines`, then `replaceInText` with skip lines
  on disk, `TextFind::dropEditsOnLines` in a tab); a file whose dismissed
  line no longer reads as its row showed is left alone whole and named
  (`changedSinceSearch`). A new search clears the dismissals
  (`search:dismissed`; checked in the `replace_in_folder` suite). Pane
  commands from the Search or TODOs window go through AppDelegate's
  `currentForPane:`, which makes the editor window key first; a toggle
  (Toggle Terminal, Toggle Browser) from there shows and focuses its pane,
  as VS Code's toggles hide only the focused view. The key monitor runs
  Global keys (Ctrl+`) for those windows too (`MCKeymapHandleGlobalKeyDown`).
- **The same on Linux** (2026-11-19, `linux/HANDOFF.md` item 116,
  `linux/src/Search.cpp`). Enter in the replace field is `fieldReturn`,
  what Enter in the query does (search, or into the results when they
  answer the query; GTK's query did that before #210); Replace All is the
  button or Ctrl+Alt+Enter (`editor.action.replaceAll`, `onWindowKey`),
  only while the replace row shows, else the bell. GTK's results are a
  flat list of lines (no file rows), so a row's × (CSS-shown on `:hover`
  and `:selected`), Delete in the list, and the right click menu's Dismiss
  take a line, and the menu's Dismiss File every line of its file; both
  record into `FolderReplaceExclusions`, Replace All copies them into its
  plan job (`excludeDismissed` on the worker) and on into
  `replaceInPaths` (`dismissedLines`, `replaceInText` with skip lines,
  `dropEditsOnLines` in a tab), and `runSearch` clears them. A single
  click on a row previews it without presenting the editor window
  (`openMatch(..., present = false)`), the list taking the keyboard, as
  the Mac's `previewHandler`; a double-click or Enter keeps it and
  presents. The search window is no application window, so no
  accelerator reached it: `runPaneKey` matches the keys of
  `kPaneCommands` (the Mac's `currentForPane:` callers) with
  `gtk_shortcut_trigger_trigger` and main.cpp's
  `runPaneCommandFromSearch` presents the editor window and runs the
  action, Toggle Terminal and Toggle Browser showing and focusing their
  pane, never hiding it. Checked in the GTK `replace_in_folder` suite.
- **Trap: a GTK search under the keyboard** (2026-11-19, a FLAKY
  `replace_in_folder` line in CI). A search clears the list at once
  (`clearResults`) and its rows come later from the worker, and the row
  that had the keyboard goes with its row; when a frame falls between the
  two, GTK's deferred focus move finds the list empty and leaves the
  window no focus at all, so the next key did nothing and a Down only put
  the keyboard back on the first row (GtkListView's arrows, from no
  focused row, start at its focus position, which the new rows reset to
  0). Alt+C from the results did it on most runs (the toggle's redraw is
  a frame), the worker's thread waiting for a CPU under load. `runSearch`
  holds the keyboard on the window's box while the list is empty
  (`ListKeyboard::hold`, as `removeRows` did) and `giveKeyboardBack` gives
  it to the first new row, or to the query when none came, unless
  something else took it meanwhile. The suite's own race: `setScope`
  searches the query already in the field, so a wait for rows passed on
  that search, and the same query typed after it was searched 0.35 s
  later (GtkSearchEntry's delay), after Enter had gone into the results;
  "ca", searched in a pause between keys, lists the rows "cat" does. The
  suite empties the query before `setScope` and waits for
  `searchedQuery()`.

## Traps

Each trap's first sentence, in the order it appears above; the whole
text is in place in its section.

- Find and replace (Mac): Traps: libc++ and libstdc++ each break JavaScript's repetition rules in places (groups unset per repetition, empty repetitions not counting), libc++ finds no `\b` at a line's end once a search starts past 0, and libc++ throws error_complexity (a silent "no match") on short exponential patterns rather than hanging, while ...
- Find and replace (Mac): Trap: CALayer copies `shadowPath`, so release the path you made, never the one the getter returns (that crashed in the next CA commit).
- The find bar in a large file: NSString gives no public way to the bytes it holds: `CFStringGetCStringPtr` returns NULL, for every encoding, for a string made with `initWithBytesNoCopy:` in ASCII (checked on macOS 27), so the reader hangs the bytes on the string (`MCSetHeldAsciiBytes`, an associated NSData that does not free them) and the find bar reads them while the string is the file's.
- The find bar in a large file: the first match found from the top read "? of …" until the count's pass reached it, and large_file's "1 of " check passed only when the whole count (a few tens of ms for one match in 100 MB) ended before the harness polled; on main after #249 it lost that race twice. A match found from the top is the first of all, so its index (0) is sent with it (`fromTop` in `searchInStages`), and the label is "1 of …" at once.
- The find bar in a large file: the old marks stayed because nothing redrew between a query's change and its results; drawing already skipped stale results, so the fix is to redraw at once and to draw only matches for the query and text as they are now (`_shownGen`, `_shownVersion`).
- Find in Folder's keys: the Search window floats over the editor window and stays key, so a command run from it through AppDelegate's `[self current]` changed the editor window's panes (Toggle Terminal showed the terminal and called `focusInput`) while every key typed after still went to Search: a walker's `python3 -m pytest -q` and Return landed in the replace field, where Return was Replace All, and a file was rewritten. A command that shows a pane must make the editor window key (`currentForPane:`), Return in a field of that window must never write files, and Dismiss must reach Replace All, which re-plans the whole folder and had ignored the rows taken out of the results.
- Where Option+Shift+Cmd+F puts the keyboard: the replace field took it with nothing searched yet, so the search typed first became the replacement; only a selection just made the query sends it there, as in VS Code.
- A GTK search under the keyboard: a search clears the list at once (`clearResults`) and its rows come later from the worker, and the row that had the keyboard goes with its row; when a frame falls between the two, GTK's deferred focus move finds the list empty and leaves the window no focus at all, so the next key did nothing and a Down only put the keyboard back on the first row (GtkListView's arrows, from no focused row, start at its focus position, which the new rows reset to 0).
