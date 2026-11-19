# The editor

Area notes moved out of `CLAUDE.md` (see its "Area notes" list). The text
below is as it stood there; section names quoted in it ("see ...") are
found through that list.

## The editor's text view (Mac)

- **The editor is TextKit 1.** AppKit gives any NSTextView subclass that
  overrides `drawRect:` a TextKit 1 layout manager, with no notification. Never
  ask a TextKit 2 view for its layout manager (it switches over).
- **Storage**: `MCSourceStorage` is an NSTextStorage over a plain
  NSMutableAttributedString that fixes attributes eagerly (only the edited
  paragraph) and skips the glyph-info fix unless glyph info is in the range,
  and the font fix (substitution, which reads every character: 1.2 s of
  each 25 MB show) over plain ASCII under fonts that have all of it.
  AppKit's storage re-fixed a 65,536-character block around the caret twice a
  keystroke and walked every run for glyph info: about 11 ms a keystroke in a
  highlighted 100,000-line file, now about 0.5 ms (opening such a file costs
  more, about 220 ms). EditorController builds the TextKit 1 stack itself and
  makes the view with `initWithFrame:textContainer:`. **Never
  `replaceTextStorage:` after the view exists**: CodeTextView, the carets and
  the folds subscribe to the storage at init and heard nothing on a new one.
  **Gaps** (2026-11-07, inlay hints): the storage answers a kern on the
  character before each `MCTextGap`, never stored, in `attributesAtIndex:`
  and its longest-range form (the typesetter reads that one, and the kern by
  name: answering only `attributesAtIndex:effectiveRange:` laid out
  nothing), and moves the gaps itself in `replaceCharactersInRange:`
  (`Lsp::inlayFollowEdit`). `attributedSubstringFromRange:` (copies, and
  NSTextView's undo, which reinserts what it kept) and
  `storedAttributesAtIndex:...` (the highlighter's compare, or every
  restyle of a hinted line set its colors again) leave them out, and
  CodeTextView hides them while writing to a pasteboard. A gap at a
  paragraph's start (offset 0, after \n, \r or U+2029; 2026-11-08) is the
  first-line indent of the paragraph style answered for its first
  character instead (`MCGapParagraphStyle`; a kern on the break before
  opens nothing), answered by name too; `fixAttributesInRange:` reads with
  the gaps hidden (fixing writes a paragraph's first style over the rest)
  and `setTypingAttributes:` puts the stored style back. **The paragraph
  style by name** (2026-11-08) comes from a run the storage keeps through
  edits (`storedParagraphStyleAt:`), not the store: TextKit asks for it at
  both ends of every range whose display it invalidates, a range reaching
  text not laid out stays pending and is asked about on every keystroke
  (after Select All, or the undo of a Replace All, which selects every
  piece), and the store finds a run by walking from the last one asked
  about, so each key walked the runs from the caret to the end and back:
  1.1 to 2.4 ms a key against 0.35 (the keystroke suite, which also fuzzes
  the run against the stored styles). TextKit's own work on the pending
  range still makes a key after Select All 1.4 to 2.2 times a fresh one.
- **Contiguous layout stays on**, except in large-file mode (below) and
  while a tab shown again far down (past 32K characters) shows:
  everything that reads positions relies on exactness. Never call `numberOfGlyphs` on the editor; lay out only up to the
  character needed. **A tab shown again far down** (2026-11-08, editor
  N2; `restoreTextPlaceOf:`, `LayoutCatchUp.{h,mm}`): its place is laid
  out alone with non-contiguous layout (3.9 s of the main thread at the
  middle of 100,000 lines before, about what showing it at the top costs
  now), and `MCLayoutCatchUp` lays out the lines above it upward from the
  pane, a piece of about 8 ms a turn, until nothing between the top of the
  text and the pane is unlaid; from then every rect from the top through
  the pane is the contiguous one (the keystroke suite compares them), and
  text below is laid out as it is reached. An edit made through the text
  view that leaves lines above the pane unlaid has them laid out as it
  ends (`NSTextDidChangeNotification`) with the pane's scroll kept, so the
  text moves as contiguous layout moves it; a new width or a jump far down
  goes to the pieces. A turn's chunks are sized from the pace of text not
  laid out at all, never from a stretch that was cheap because most of it
  was laid out already: sizing from the last piece's cost grew pieces over
  such a stretch to 278,000 characters and one turn to 149 ms
  (2026-11-09). **Traps**: turning `allowsNonContiguousLayout` off
  throws all layout away (0.7 s to lay out to the middle again), so the
  flag stays on until the tab leaves (`displaySourceEditable` and
  `makeEditorReadOnly` set it per tab). With it on, NSTextView scrolls to
  keep the pane's text still as layout above it changes, and an edit that
  adds a line above the pane leaves the rest estimated (the pane's text
  moved 4 points up, not 18 down) until laid out. After each hole filled
  TextKit looks up attributes at the pane and the caret, and with the
  highlighter's 520,000 color runs a lookup far from the last walks the
  runs between: the catch-up costs 1.5 to 3 times the CPU contiguous
  layout did (1.8 s in idle turns against a 1.2 s stall for 50,000 lines
  on a quiet Mac, 6 to 7.5 against 2.3 on a loaded one), and small pieces
  far more. The same lookups after each storage edit made Replace All of
  20,000 matches 4.3 s against 0.6, so `replaceRanges:` posts
  `MCTextViewWillReplaceManyRangesNotification` from 64 ranges on and the
  catch-up goes back to contiguous layout first (1.7 s, the pane then laid
  out from the top once); the workspace edit applier has its own loop and
  does not post it yet. **The tab's text is copied when read**
  (2026-11-08, `MCTab followStorage:`): a copy of the store's CFString is a
  memcpy while it is 8-bit (0.15 ms at 2.4 MB) and a scan of every
  character once a non-ASCII one has been in it, even undone (1.3 to 3 ms;
  AppKit's NSBigMutableString shares its characters), and
  `textDidChange:` made one a keystroke for `sourceText`. Now it only
  marks the tab; `sourceText` copies at its first read after a change
  (a new object per change, so pointer compares still work), and the
  storage's `willReplaceAllCharacters` copies it before anything replaces
  the storage whole. What runs every keystroke must not read `sourceText`:
  `currentText` (no copy, never kept), `hasSourceText`, `textGeneration`.
- **Trap: a Markdown page's text blocks outlive it** (2026-11-07). The
  preview shares the editor's view, and once its quotes, code boxes, h1/h2
  rules or tables are laid out the layout manager keeps a block table
  (`_blockRunsArray`) after the text is replaced; while it exists TextKit
  recalculates the used rect after every edit over every laid-out line
  fragment instead of from the first and last (`_enableTextViewResizing`,
  `_recalculateUsageForTextContainerAtIndex:`): 1.5 to 10 ms a key at
  100,000 lines, 16 to 48 ms at 25 MB. A fresh layout manager or text
  container does not help while the page is still in the storage.
  `renderMarkdown:` marks it (`layoutHeldTextBlocks`) and
  `displaySourceEditable` takes the layout manager off the empty storage
  and puts it back (`forgetTextBlockLayout`), which drops the table and
  keeps the object, its delegate and settings. The keystroke and
  large_file suites type in a window a page used.
- **Programmatic edits** go through `CodeTextView replaceRanges:withStrings:
  actionName:` (Lsp.h): `replaceSourceWithText:actionName:` (Format
  Document, the inline agent, a clean tab following the disk, a pending
  sort), several carets (`MCMultiCarets applyEdit:`) and Replace All; the
  workspace edit applier and completion's extra edits (Lsp+WorkspaceEdit.mm,
  Lsp+Completion.mm) have the same shape. One `shouldChangeTextInRanges`, each range its own storage edit,
  last first, one `didChangeText`, so layout runs once and the highlighter,
  folds, squiggles, gutter, Go Back and server follow each edit. Never wrap
  them in `beginEditing`/`endEditing`: the storage then reports one edit from
  first to last, folds between open, squiggles stretch over it, and the
  highlighter relexes all of it (a quote at two carets 99,800 lines apart
  took 4.5 s; 3 ms now). The undo step is the view's own (allowsUndo off
  around shouldChangeTextInRanges), putting the old text back as plain
  strings through the same method: NSTextView's inserts each as an
  attributed string, splitting the attribute runs one at a time, and
  undoing a letter typed at 20,000 selections ran for minutes. Its handler
  checks that each range still holds what was inserted (allowing one
  change made outside the undo history to have moved them) and otherwise
  beeps and logs (`os_log`, subsystem ai.ible.mutible), never putting
  old text over something else; it gave up silently on any length change.
  From 64 ranges on, the glyphs from the first range to the last are
  invalidated once before the edits (2026-11-07): each storage edit
  otherwise replaced its glyphs in the layout manager's glyph tree at a cost
  growing with the glyphs made, and Replace All of 20,000 matches in a
  100,000-line file laid out to its end took 3 to 8 s (0.6 to 1.2 s now).
  Nothing is lost by it: TextKit 1 keeps one soft-invalidated span, so the
  layout between the first edit and the last is redone anyway. The
  highlighter keeps its pending ranges apart (`_pending`, distances from the
  end) and restyles each in its own attribute edit. Select All Occurrences,
  Cmd+D and Add Cursor stop at 10,000 carets (VS Code's limit); typing at
  10,000 in a 100,000-line file still costs about a second, most of it the
  flat string's memmove per edit. The caret and top
  character are mapped by counting non-whitespace characters within a hunk;
  hunks with only blank lines between are joined. Small edits (Cmd+/, F9) call
  `breakUndoCoalescing`, so each is its own undo step.
- **No smart copy and paste** (2026-11-14): `smartInsertDeleteEnabled` is
  off on the editor, since AppKit's default put a space before a word
  pasted after a dot ("obj. thing"); editor_keys checks paste and cut.
- `CodeTextView` hooks: `onFocusLost`, `onMouseHover`, `onPreviewDoubleClick`,
  `onFindAction`/`validateFindAction`, `drawBehindText`. The ruler
  (`editor.ruler`: a column, a list such as 80,120, or off, 100 by default;
  `editor.ruler-color`) is a 1 pt backing-aligned line at container origin +
  padding + column times the width of "0", drawn in
  `drawViewBackgroundInRect:`, none while read-only.
- **Line numbers** (`MCLineGutter`): a vertical NSRulerView; each logical line
  numbered on its first fragment, the carets' lines in the active color, a dot
  for errors and warnings, the change lane (`kChangeLane`, while
  `editor.git-gutter` is on: `GitGutter`'s bars, moved by `QuickDiff::shift`
  in `storageEdited:` and cleared by a whole new text), the fold lane
  (`kFoldLane`), and leftmost the test lane (`kTestLane`, while
  `testLane`: `TestGutter`'s marks, moved with their lines the same way).
  Only fragments on
  screen are visited and numbers come from a `LineIndex` fed by storage edits
  (with the characters either side, since a line ends after '\n', U+2029 or
  a '\r' not before '\n', as in TextFind; the folds count lines the same way).
  Width by digit count (three at least); click, drag and Shift+click select
  lines; shown only while editable. Settings: `editor.line-numbers` (on, off,
  relative), `editor.gutter-background`, `editor.line-number-color`,
  `editor.line-number-active-color`. Traps: NSScrollView lays a ruler out as a
  content inset of a clip view that still spans the width, so `PanelScrollView
  tile` subtracts the insets; never change `ruleThickness` inside a storage
  edit (tile asked for text that was gone and threw), so it changes on the next
  run-loop turn.
- **Brackets**: the bracket by the caret (the one before first) and its partner
  get boxes in `editor.bracket-match`; strings and comments skipped by the
  highlighter's tokens within 20,000 units; nothing lexed unless a bracket
  touches the caret. Go to Bracket is Shift+Cmd+\.
- **Zoom** (EditorFont.mm): Cmd+=, Shift+Cmd+=, Cmd+-, Option+Cmd+0, Cmd+keypad
  0; Cmd+0 stays Focus File Tree. `MCEditorFont()` is `editor.font-size` (13)
  plus `MCEditorZoom` (user defaults, every window, never in the settings
  file); `MCZoomScale` is (13 + zoom) / 13 for the Markdown preview
  (re-rendered keeping its place) and the edit popovers. **The terminals
  have their own zoom** (2026-11-08, the owner's report: shrinking one
  shrank the other): `MCTerminalZoom` (defaults, every terminal in every
  window; seeded once from the editor's zoom), added to
  `terminal.font-size`, changed by the same keys while a terminal (grid or
  line mode) has the keyboard: the registry's
  `workbench.action.terminal.fontZoom*` rows are `When::Terminal` and the
  editor's are `When::NotPageOrTerminal`, so neither overlaps; a Terminal
  command's key another command also has stays off its menu item
  (`MenuShows`), and the key monitor runs it (`MCTerminalZoomDidChange`).
  In a page (the browser pane, its toolbar, a PDFView, the LaTeX preview)
  Cmd+=, Cmd+- and Cmd+0 are Page Zoom instead (2026-11-06,
  `EditorController+ZoomFit.mm`, `PageZoom.{h,mm}`): the registry's
  `When::Page` against `When::NotPage` (Focus File Tree) and
  `NotPageOrTerminal` (the editor's zoom), which do not overlap; a page command's keys stay off its menu item
  like a tree command's, and the key monitor runs them while a page has the
  keyboard, or after a scroll or click landed in one with no key since
  (`MCPagePointedAt`). Safari's steps; the user's page keeps a zoom per site
  (`MCBrowserZoom` in the defaults); Zoom to Fit Width reads clientWidth and
  scrollWidth in the isolated world at 100% first. Element full screen is on.
- **Word wrap**: `editor.word-wrap` (on), turned over for the session by
  Option+Z (`MCEditorWordWrap`, EditorFont.mm); `applyWordWrap` runs from
  relayoutRightArea, applySettings and showing source text; a read-only view
  (a preview) always wraps. Not wrapping, the text view is horizontally
  resizable and `PanelScrollView tile` keeps it at least the pane's width.
  The compare view never wraps (its rows stay level), and a CSV table scrolls.
  **Wrapped lines keep their indentation** (2026-11-18, round 9;
  `editor.wrapping-indent` same, indent, none; `WrapIndentContainer.{h,mm}`,
  core `WrapIndent.h`, `wrap-indent:*`): the editor's text container (and
  the split group's mirror) answers each line fragment's rect, and for a
  line that does not start a paragraph it answers a rect set in by the
  paragraph's leading spaces and tabs, measured in the paragraph's font with
  its paragraph style's tab stops, so the typesetter breaks the line inside
  it. No attribute is stored, so the text, undo, copies and the carets are
  untouched. The container still says it is a simple rectangle: **trap**,
  a container that says otherwise loses non-contiguous layout (laying out
  near the end of 100,000 lines took 3 s, not 3 ms), and TextKit asks a
  simple one for every line's rect anyway. It reads a paragraph once (its
  bounds and indent are kept until the next edit, which the layout
  manager's `processEditing...` reports), set before the source goes in
  (`displaySourceEditable`) and turned off before a page does
  (`makeEditorReadOnly`; a Markdown page has its own indents), so neither
  lays out twice. Past half the line's room it falls back to none, as VS
  Code does. zoomfit checks the x of each continuation against the first
  non-blank glyph, the three modes, the fallback, a click and a copy, and
  the CPU of a space typed into a wrapped line's indentation in 20,000
  lines. **Linux** (2026-11-19, `linux/src/WrapIndentTags.{h,cpp}`,
  HANDOFF item 114): GtkTextView has no per-fragment hook, so each source
  line with leading blanks and text after them carries one tag of the
  shared table, `mutible-wrap-<columns>` (tabs to Pango's 8-column stops),
  whose negative `indent` is a hanging indent; the tags' pixels come from
  `WrapIndent::continuationIndent` for the mode, a space in the view's
  font and the view's room (`Editor::updateWrapIndent`, after an
  allocation or a zoom, in an idle), so a zoom or resize changes tags, not
  lines. An edit retags only the lines it touched (none when the leading
  width is unchanged); opening a file tags the lines on screen at once and
  the rest in 5 ms idle slices; large-file mode and none tag nothing.
  The GTK suite word_wrap checks it.
- **Maximize Pane** (Ctrl+Cmd+M; VS Code's is a Cmd+K chord): `_maximizedPane`
  in relayoutRightArea shows only that pane; showing or focusing another
  (`leaveMaximizedPaneFor:`) ends it.
- **An action AppKit answers itself never reaches the controller.** Cmd+B was
  `toggleSidebar:`, which NSSplitView and NSWindow implement on current
  macOS; the split view is in every pane's responder chain, so Cmd+B did
  nothing anywhere (the owner's report, 1.5.6). It is
  `toggleSidebarVisibility:` now, and the zoomfit suite checks every
  registry selector against AppKit's views and windows.
- `_liveHighlight` must be off whenever the storage holds something that is not
  source (previews, messages); set it before every `setAttributedString:`.
- **Large-file mode** (2026-11-06, +LargeFile, `LargeFile.h`): VS Code's
  thresholds, a text over 20 MB (UTF-16 units) or 300,000 lines, with
  `editor.large-file-optimizations` on, read when the tab loads (`MCTab
  largeFile`): no highlighter, no fold regions (`folds.fileName` nil), no
  bracket boxes, and no language server (`languageServerPath:` for
  `documentOpened:`, nil from `bufferText`; never past VS Code's 50 MB sync
  limit either). The status bar's "Large file: features off" turns them on
  for the tab (`largeFileFeatures`). A file over 1 GB opened by hand
  (`openFileAtPath:`, a new tab only) asks Open Anyway or Cancel
  (`kAskBeforeOpeningBytesMac`, VS Code's limit; 50 MB until 2026-11-16,
  when an 82 MB log from Finder stopped at the question though it then
  opened in 0.7 s, and Zed asks nothing; the GTK port still asks past
  50 MB). Measured:
  25 MB opens in about 0.25 s of CPU (2.2 s with the mode off) and types
  at about 5 ms a key; 60 MB in 0.7 s. **Its layout is non-contiguous**
  (2026-11-08, set in `displaySourceEditable`, cleared by
  `makeEditorReadOnly` and when the note turns features on, which lays
  out above the top line then and keeps the place): showing the tab again
  at line 300,000, Go to Line or a search hit lays out a screen there, not
  every line above it (13 to 18 s of CPU before, 0.25 s now). Rects far
  from what is laid out are estimates, which nothing the mode keeps
  needs; a from-the-top `ensureLayout...` (`MCTextViewRectForCharacter`,
  `restoreTextPlaceOf:`, the zoom, notes) starts near its target when
  `allowsNonContiguousLayout` is on. The scroll bar's thumb and a jump
  far down need the owner's eyes on a real screen. **Trap: word wrap stays as set**, unlike VS
  Code: a TextKit view that does not wrap lays out all of its text in
  `sizeToFit` to learn its width (5.5 s for 25 MB, nine tenths of it there).
  Known: a keystroke in a 5,000,000-character line costs about 0.35 s, all
  of it TextKit typesetting the paragraph again; the highlighter reads 3
  units of it. Switching away from a large tab laid it all out as the line
  numbers hid (1.3 to 5 s at 25 MB); `makeEditorReadOnly` (resetViewMode,
  showActiveTab) now empties a text over 1 MB first (60 ms); what shows
  next calls `documentOpened:` in the same turn, which drops the emptying
  before it is sent (large_file checks no didChange goes).
  The 16 to 48 ms keys in a window
  earlier suites had used were a Markdown page's text blocks (see "The
  editor's text view").
- **Trap: anything that lays out all of a large text stays.** Two did on
  every open after a file with diagnostics or a preview: `needsDisplay =
  YES` on the text view, which NSTextView answers by laying out every glyph
  of its bounds (CodeTextView's `setNeedsDisplay:` marks only the visible
  and prepared rects that way, and since 2026-11-18 the bounds too with
  `avoidAdditionalLayout:YES`, which lays out nothing: tiles past them
  kept an older page's ink, see the Markdown notes; clearing
  squiggles took 2.5 s at 25 MB), and
  the gutter appearing after the text went in, which narrows the view and
  lays out all of it for its height (`displaySourceEditable` shows the
  gutter over an empty storage first for a text over 1 MB). Check with
  `MC_GUI_SAMPLE` after another suite, not only alone (`SUITES="language
  large_file"` caught both).

## Folding (Mac)

- `Fold::compute` finds regions as whole lines: brackets for the C-like
  languages and .bib (the closer's line stays in view when only closers precede
  it, so `} else {` reads as in VS Code; brackets in strings and comments do
  not count), indentation for Python, YAML and the rest (trailing blanks
  excluded; lines starting inside an earlier string or comment neither open nor
  close), headings, fences and display math in Markdown, sections and
  environments in LaTeX, block comments, docstrings, comment runs, and
  `#region`/`#endregion` in the usual spellings. `sanitize` nests any list (the
  server's ranges too). `Fold::Collapsed` keeps closed folds as UTF-16 spans,
  so they follow edits; an edit touching what one hides opens it.
- `MCFolds` (FoldView.mm) is the layout manager's delegate: hidden characters
  get null glyphs except the break ending the first line, which becomes a blank
  as wide as the placeholder the view draws. The text never changes, so the
  gutter, undo, the highlighter, copy (copies what is hidden) and carets see it
  whole; arrows step over null glyphs; squiggles under folds are not drawn.
  Backspace after or Delete before a placeholder opens the fold; Return after
  it makes a line below. A selection landing inside a fold opens it
  (`setSelectedRanges:`), which is how Find, Go to Line, definition, F8 and
  Select All Occurrences reveal their target. Folds are per tab and forgotten
  on a reload from disk.
- Regions recompute off the main thread 0.3 s after typing (generation
  counted); `textDocument/foldingRange` is asked at 0.6 s and wins for its
  text. Chevrons show while the pointer is over the gutter.
- Keys: VS Code's Option+Cmd+[ ] are mutible's terminal switching and its
  other fold keys are Cmd+K chords, which mutible has none of, so View >
  Folding is Control+Command plus the chord's second key ([ ] L 0 J / and 1 to
  7). Control+[ types ESC: test these as real key codes.
- Trap: one region at a time was quadratic (6 s and 18 s for Fold All and
  Unfold All of 50,000); they go in as one sorted merge and one invalidation
  (`collapseAll`).

## Several carets and line commands (Mac)

- NSTextView keeps one empty selection, so carets live in `MCMultiCarets`
  (UTF-16); the view's selection holds the non-empty ones or the first caret,
  and the view draws every caret after `drawRect:`. Any change not made by the
  carets (a click, undo, reload, tab switch, Find), Esc, or an unknown command
  (run at the first caret) ends them; scrolling passes. Edits go through
  `replaceRanges:withStrings:actionName:` as one undo step; the server
  sees no single typed character (no completion); the electric dedent is off
  while they apply (`multiCarets.applying`); typing never crosses a line break,
  so a column stays a column. The arrows, Backspace, Delete, Return, Copy
  and Cut, and the line commands read the text through a
  `MultiCaret::Text` over the storage (`TextOf`, blocks of 1,024 read near
  the carets), not a copy of the whole buffer per key.
- Column: Cmd+Shift-, Option- or middle-button drag (`MCMultiCarets mouseDown:`
  and `otherMouseDown:`, first in CodeTextView's), each line's caret from
  `characterIndexForInsertionAtPoint:` at its first fragment; Cmd+Shift clicks
  pair up (the owner's binding), the anchor holding while carets and length
  are as the last press left them (`_anchorCarets`); Shift+Option click or
  drag is VS Code's box from the caret's selection start (or the live
  anchor) to the pointer; Option+click adds or removes a caret. VS Code's
  own Cmd+Shift click is a Shift click. **Trap (2026-11-06)**: the window
  server counts clicks by time alone (HIDClickSpace is (0,0)), so a second
  click within 0.5 s is clickCount 2 wherever it lands; the old `clickCount
  > 1` bail sent the owner's quick second Cmd+Shift click to NSTextView. A
  press counts as a double-click only on the character the last press hit
  (VS Code's `trySetCount` does the same). Offscreen, neither `[window
  sendEvent:]` nor CGEventPostToPid delivers a mouse-down to the view;
  `MultiCaretClickSuite` hit-tests and calls the view with CGEvent-made
  events carrying the real click counts.
- Cmd+D (word at an empty caret by `MultiCaret::wordAt`, then the next
  occurrence after the last added, wrapping, case-sensitive, whole-word only
  when begun at an empty caret), Shift+Cmd+L, Option+Cmd+Down/Up (a caret a
  visual line away at `_goalX`). VS Code's Mac keys (2026-11-08; until then
  Cmd+Up/Down moved lines and Shift+Cmd+Up/Down copied them): Shift+Option+
  Up/Down copy lines; Option+Up/Down move lines (neighbouring carets as one
  block; nothing moves at an edge); Cmd+Up/Down and Cmd+Home/End go to the
  file's ends, Shift selecting (AppKit's own selectors; with several carets
  `Move::DocumentStart`/`DocumentEnd` leave one, or one selection from the
  farthest anchor, as VS Code's merge does). Shift+Option+arrows are caught
  in CodeTextView by selector and arrow key code from the event `keyDown:`
  recorded (`_keyEvent`; `NSApp.currentEvent` is unset for a direct
  `keyDown:`); Option+arrows by key code in `keyDown:`. Not menu items, which would take
  keys from the find field and commit box. Cmd+Delete deletes to the line's
  start (the tree takes it as Move to Trash in its own `keyDown:`; as a menu
  key it trashed the tree's selection from the editor).
- **Home and End** (2026-11-18, a grader's walk: End scrolled, AppKit's
  binding, so "End, Left Left" typed a line up): VS Code's cursorHome and
  cursorEnd, taken by key code in CodeTextView's `keyDown:` (plain and
  Shift; Cmd is the file's ends above; Page Up and Down still scroll),
  `MCMultiCarets homeOrEnd:extend:`, core `MultiCaret::homeTarget`,
  `endTarget`, `homeCarets`, `endCarets` (`multicaret:home-end`, `make
  guitest SUITES=editor_keys`). Home: first non-blank, then column 0 (an
  all-blank line: column 0); End: the line's end. With word wrap a row is
  the line fragment (`MCRowAt`; a caret at a fragment's start is on it):
  End goes to the row's end first, which for a row that wraps is before
  its last character (the space it broke at), so the caret is drawn at
  that row's edge and needs no upstream affinity, then the line's end; Home
  on a later row to its start first. One caret's head is not public in
  AppKit: `soleHead` steps Shift+Right and back (Shift+Left at the text's
  end) and sees which end moved. With Shift the head is carried by
  AppKit's own moves (paragraph start or end, then single steps from the
  nearer of head and line start, `extendSoleFrom:`), since a selection set
  outright makes AppKit extend from its start: Shift+Home then Shift+Right
  must give a character back. **Linux** (2026-11-19, HANDOFF item 114):
  GTK's own Home and End went to the display line's start and end (no
  first non-blank); `Carets::onMoveCursor` now takes
  `GTK_MOVEMENT_DISPLAY_LINE_ENDS` at one caret or several
  (`Carets::homeOrEnd`, the same core calls), a row being GTK's display
  line (`Carets::rowAt`: `gtk_text_view_backward_display_line_start` to the
  next one's start). GtkTextBuffer keeps the anchor itself, so Shift needs
  none of the Mac's head-finding. A long line shown a part at a time
  follows the caret (`LongLines::followCaret`). `cursorHome` and the rest
  are Linux rows (home, end, with Shift), listed, not actions;
  `./build/mutible-guitest editor_keys` checks them.
- **Words** (2026-11-08; a double-click on `arr` in `arr.size` took both,
  AppKit's UAX 29 words keeping a period between letters): in source text
  every word rule is `MultiCaret`'s `charClass`, VS Code's default
  editor.wordSeparators in every language. NSTextView asks the storage:
  `MCSourceStorage doubleClickAtIndex:` (`MultiCaret::wordSelection`, VS
  Code's WordOperations.word; asked at both ends of a drag by words) and
  `nextWordFromIndex:forward:` (Option+arrows and Option+Delete, AppKit
  keeping the anchor), the same words several carets' moves and Cmd+D use.
  A read-only text view (a preview) keeps AppKit's words.
- Up and Down with many carets walk the layout manager a line at a time (goal x
  by binary search over glyph starts; `glyphIndexForPoint:` was most of the
  time); before, carets on lines not laid out got empty rects and merged.
  Drawing places only carets among the visible characters (placing all laid out
  the file: seconds a frame for 50,000).
- **Auto-closing pairs** (2026-11-07, `editor.auto-close`, on): VS Code's
  rules at its defaults (autoClosingBrackets/Quotes/Comments
  "languageDefined", autoClosingOvertype and autoClosingDelete "auto",
  autoSurround "languageDefined"), in the core (`AutoClose`, each language's
  pairs from VS Code's language-configuration.json; Python's """ closing on
  its third quote and LaTeX's brackets-only are the two departures). Every
  single typed character, at one caret or several, goes to `MCMultiCarets
  insertText:replacementRange:` first, which asks `MCAutoClose`
  (EditingCommands.mm; the controller is its `MCAutoCloseHost`) for an
  overtype, a close or a surround; never while marked text composes or for
  a replacement range (the accent menu). Backspace asks it in `doCommand:`
  before the multi-caret check. A close is two changes at one place (the
  typed character, then its closer), so applied last first the typed
  character is the last single-character storage edit and the language
  server still takes "(" as signature help's trigger; an overtype only
  moves the carets (no undo step). The `Tracker` follows storage edits
  (`NSTextStorageDidProcessEditing`) and drops a keystroke's closers once a
  caret leaves its pair or the pair spans lines (checked on every selection
  change and `setCarets:`). Surround (an opener over every caret's
  selection) uses the language's surrounding pairs, not only `( [ { " '
  \``; none in JSON or Makefiles, `* _ ~ $ <` in Markdown. GUI trap: a
  simulated key press that changes nothing, wrapped in `MCGuiUndoGroup`,
  leaves an empty undo group on top that a real key press never opens.
- Line commands: Cmd+L (`expandLineSelection`), Ctrl+Shift+K (`deleteLines`),
  Cmd+C and Cmd+X with nothing selected take whole lines (`lineCopy`) and mark
  the item `ai.ible.mutible.line-copy`; pasting such an item with nothing
  selected goes above the caret's line (`pasteLinesAbove`). Only while
  editable; `copy:` and `cut:` validate with an empty selection there.

## Indentation and sorting (all three ports)

- `AutoIndent::scan` reads from the top to the caret: strings (Python prefixes
  and triple quotes, JS templates, Go raw strings; Rust lifetimes are not
  strings), comments (`#` only at a word start in shell and YAML; no `//` in
  CSS or JSON), and open brackets with their lines. Languages: Python, CLike,
  Yaml, Generic (brackets only), Plain (keep the indentation). `ScanCache`
  keeps the scanner's state at line starts about 4,096 characters apart (a
  state depends only on what precedes it) and `detect`'s answer; the Mac keeps
  one per text storage, fed from `NSTextStorageDidProcessEditing`, and every
  edit must be reported.
- Python Return, in order: in a string keep the indentation; in a bracket,
  `(|)` opens the pair, a moved closer takes the opener line's indentation,
  text after the opener aligns with it, else a hanging indent; otherwise a
  trailing backslash indents once, a block colon indents,
  return/pass/break/continue/raise dedent (only with nothing moved down), else
  the statement's first line's indentation. CLike: one unit after an opener,
  else the line's own or the bracket opener line's; " * " in `/*`.
- Electric dedent (`typed`): a closer alone on its line takes its opener's
  indentation (for `(` and `[` only when the opener ended its line); in Python
  the colon of else, elif, except, finally or case moves the line to the
  nearest shallower statement start of the right kind.
- Indent unit (`detect`): tabs if more lines start with one, else the commonest
  step of 4, 2, 8 or 3 after a block opener; defaults 4, 2 in YAML, tabs in Go
  and Makefiles; `.tsv` never soft tabs. No settings key: the file decides.
- `KeepSorted` is keep-sorted's Go code (v0.10) step for step, including the
  regex options through std::regex with a small RE2 translation (`\p`, `\Q` and
  the like leave a block alone with a message); a marker without its partner
  leaves the file alone; a block whose groups would split a nested block is
  left alone (keep-sorted scatters the markers). Fuzzed against the real tool;
  its golden files are `tests/keepsorted` (Apache 2.0). An outermost block
  sorted to nothing leaves one empty line, as keep-sorted does.
- F9 sorts a selection of two or more lines (stable, by text after leading
  spaces) or the innermost block; Shift+F9 every block;
  `editor.keep-sorted-on-save` (not a tab showing a preview). Each edit is one
  replacement and undo step (Mac: `breakUndoCoalescing`).
- Ports call in from their key paths: the Mac's `textView:doCommandBySelector:`
  through `MCIndentCommand`, Linux `onViewKey` unless completion is open,
  Android `CodeEditText` (keys, committed "\n" and "\t",
  `deleteSurroundingText`, the symbol row's Tab).

## The palette, symbols and Go Back (Mac)

- Shift+Cmd+P opens it with ">", Cmd+P empty (Quick Open), Ctrl+G with ":",
  Shift+Cmd+O with "@" ("@:" groups by kind), Cmd+T with "#"; the first
  character decides (`CommandPalette::parse`). Ctrl+G goes to the terminal grid
  while it has the keyboard.
- `MCPaletteOverlay` is a view over the window, not a window, so the editor
  window stays key and validation sees the menu bar's responder chain. It
  remembers the first responder (a field, not its field editor, with its
  selection) and puts the keyboard back on Esc, a click outside, or running
  something; the keyboard moving elsewhere closes it.
- Commands are read from `NSApp.mainMenu` on each open (submenus `update`d;
  enabled, visible, non-alternate items as "Menu > Item", with the first key in
  effect). Return sends the action with `[NSApp sendAction:to:from:]`, the item
  as sender, on the next run-loop turn. Context-menu-only commands are not
  found; the File and View menus repeat them. `CommandPalette::rank`: a
  subsequence aligned by a small DP favouring word starts and runs, in tiers,
  then recent (`MCCommandPaletteRecent`), score, menu order.
- Quick Open: `listFiles` breadth first on a background queue (Find in Folder's
  skips and ignore files, dotfiles listed, symlinked folders once, 20,000
  files, cancelled on close); the name
  matches before the path; recent tabs and `MCQuickOpenRecent` first. Go to
  Line takes "42", "42:7" or "42,7".
- Symbols: `textDocument/documentSymbol` from the main server (`Lsp::Symbol`, a
  tree flattened parent first; `symbolAt` selects the caret's), else
  `SymbolOutline` (Markdown headings, LaTeX sections, Python def and class, the
  C family's functions and types). Moving through the list shows each symbol
  without taking the keyboard; Esc puts selection and scroll back.
  `workspace/symbol` goes to every running server that offers it, older
  requests cancelled, answers merged.
- **The folder picker** (2026-11-16, the owner's ask: Finder's open
  panel "always sucks"; `FolderPicker.{h,cpp}`, `folderpick:*`, `make
  guitest SUITES=folder_picker`): Open Folder… (`openFolder:`, Cmd+O) and
  the Dock's Open Folder… (`openFolderInNewWindow:`) show the palette with
  an `MCFolderPick` (`MCShowFolderPicker`, `MCPaletteContext folderPick`),
  never NSOpenPanel, except Choose in Finder…, its last row, and the
  Dock's with no editor window. Rows are the core's: a name is matched
  (`rankFiles` on the paths spelled with ~) against the recent folders
  then the base folder's (the window's, else home) subfolders; text with
  a '/' or starting with "~" is a path whose folder's subfolders are
  matched by name, Open This Folder first when the text ends in '/'
  (Return opens it), else after the matches when the text names a folder
  the listing has. Hidden folders only after a typed dot. Folders are read
  with `FolderPicker::listSubfolders` (readdir, stat only for links) on a
  global queue, kept per palette by path, the reading of a folder no longer
  typed cancelled. Tab (`completeSelectedFolder`) and '/' typed after a name
  (`controlTextDidChange:`, `slashCompletes`) put the selected folder in
  the field (`FolderPicker::complete`, keeping the text's own spelling of
  the folder before it); Cmd+Return reaches the overlay's
  `performKeyEquivalent:` ahead of Insert Line Below's menu key. Open
  Folder… now asks about unsaved tabs after the choice (`switchToFolder:`),
  so a cancelled pick asks nothing. A pick or the folder picker skips the
  menu walk the palette makes for commands.
- Go Back and Forward (Ctrl+-, Ctrl+Shift+-): `MCNavigator` over `NavHistory`
  (places within ten lines are one entry; a new jump drops what lay ahead; 50
  entries; entries follow renames and leave with deleted files). Recorded: tab
  switches (`tabWillSwitch`/`tabDidSwitch`), every LSP jump
  (`LspSession.aroundJump`), Go to Line, terminal links, search and TODO
  results, symbols, and a caret moving over ten lines without the text changing
  (length-changing selections ignored). In the grid terminal Ctrl+- belongs to
  the program.

## Traps

Each trap's first sentence, in the order it appears above; the whole
text is in place in its section.

- The editor's text view (Mac): **Traps**: turning `allowsNonContiguousLayout` off throws all layout away (0.7 s to lay out to the middle again), so the flag stays on until the tab leaves (`displaySourceEditable` and `makeEditorReadOnly` set it per tab).
- The editor's text view (Mac): **Trap: a Markdown page's text blocks outlive it** (2026-11-07).
- The editor's text view (Mac): Traps: NSScrollView lays a ruler out as a content inset of a clip view that still spans the width, so `PanelScrollView tile` subtracts the insets; never change `ruleThickness` inside a storage edit (tile asked for text that was gone and threw), so it changes on the next run-loop turn.
- The editor's text view (Mac): **Trap: word wrap stays as set**, unlike VS Code: a TextKit view that does not wrap lays out all of its text in `sizeToFit` to learn its width (5.5 s for 25 MB, nine tenths of it there).
- The editor's text view (Mac): Word wrap, wrapped lines keep their indentation: **trap**, a container that says otherwise loses non-contiguous layout (laying out near the end of 100,000 lines took 3 s, not 3 ms), and TextKit asks a simple one for every line's rect anyway.
- The editor's text view (Mac): **Trap: anything that lays out all of a large text stays.** Two did on every open after a file with diagnostics or a preview: `needsDisplay = YES` on the text view, which NSTextView answers by laying out every glyph of its bounds (CodeTextView's `setNeedsDisplay:` marks only the visible and prepared rects that way, and since 2026-11-18 the bounds too with `avoidAdditionalLayout:YES`, which lays out nothing: tiles past them kept an older page's ink, see the Markdown notes; clearing ...
- Folding (Mac): Trap: one region at a time was quadratic (6 s and 18 s for Fold All and Unfold All of 50,000); they go in as one sorted merge and one invalidation (`collapseAll`).
- Several carets and line commands (Mac): **Trap (2026-11-06)**: the window server counts clicks by time alone (HIDClickSpace is (0,0)), so a second click within 0.5 s is clickCount 2 wherever it lands; the old `clickCount > 1` bail sent the owner's quick second Cmd+Shift click to NSTextView.
- Several carets and line commands (Mac): Trap: AppKit's selection affinity is not the selection's direction (Shift+End to a wrapped paragraph's end left it upstream), and a selection set with `setSelectedRange:` extends from its start whatever affinity it is given; keep AppKit's anchor by moving with its own `...AndModifySelection:` methods (`extendSoleFrom:`). Those paragraph moves never cross the anchor: from {10, 5} with the anchor at 15, moveToEndOfParagraphAndModifySelection: gave {10, 15}, so a move past the anchor starts from an empty selection there (CI's editor_keys caught it).
- Several carets and line commands (Mac): GUI trap: a simulated key press that changes nothing, wrapped in `MCGuiUndoGroup`, leaves an empty undo group on top that a real key press never opens.
