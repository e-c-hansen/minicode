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
  Match on one of its rows says why); clicking its row opens it as the
  editor opens any file not UTF-8, in Quick Look (the editor has no
  Windows-1252 yet); the files left out counted and named (`skipped`,
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

## Traps

Each trap's first sentence, in the order it appears above; the whole
text is in place in its section.

- Find and replace (Mac): Traps: libc++ and libstdc++ each break JavaScript's repetition rules in places (groups unset per repetition, empty repetitions not counting), libc++ finds no `\b` at a line's end once a search starts past 0, and libc++ throws error_complexity (a silent "no match") on short exponential patterns rather than hanging, while ...
- Find and replace (Mac): Trap: CALayer copies `shadowPath`, so release the path you made, never the one the getter returns (that crashed in the next CA commit).
