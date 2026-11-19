# Markdown, math and Mermaid

Area notes moved out of `CLAUDE.md` (see its "Area notes" list). The text
below is as it stood there; section names quoted in it ("see ...") are
found through that list.

## Markdown

- **Parser** (rewritten 2026-09-29 to CommonMark and GitHub's rules, after the
  old one italicized `max_len` and `2 * 3 * 4`): `analyze()` reads lines into
  blocks and classifies each (`MdLine`); the inline pass reads a whole block,
  so emphasis spans lines, with the delimiter rules (underscores inside a word
  never emphasise; stars do, as on GitHub). Only a few HTML tags mean anything;
  others are dropped, their text kept. CommonMark's HTML blocks (2026-11-06,
  `htmlBlockStart`) are read by `Inline::parseHtml`, tags and entities only
  (a block-level tag breaks the line; a `<h2>` line inside stays a heading;
  `<script>`, `<style>`, `<?`, `<!X` hidden; `<pre>` as code), `MdLine::Html`
  and `MarkdownEdit::Block::Html` (appended to both enums; Linux's and
  Android's title lists follow). GFM footnotes: definitions are `Block`s with
  `note`, left out of the page and emitted after it (a rule, then a numbered
  list, `MdRun::footnotes`) in the order `Notes::refer` numbers them;
  references are runs with `footnote`; `MdRun::anchor` names what `#fn-N` and
  `#fnref-N` land on (the Mac and Linux record them; Android does not yet).
  A definition's contents are four columns in, as an item's past its
  marker: a fence there is the note's code block, eight columns after a
  blank line indented code (`codeIndent`); the ↩ links follow a note's last
  code block on a line of their own.
  `<details>` (2026-11-15, `md:details`): the parser carries a stack of
  open blocks from block to block (`Details` in the Context, a body's
  Markdown sits between the HTML blocks holding the tags) and stamps each
  run's `details` depth, `detailsLine` (its tag's line, the block's name),
  `summary` and the tag's `open`; the break before `</summary>` ends a
  paragraph, so a summary is a line of its own. `MarkdownParser::fold`
  (Mac only so far) drops what closed blocks hide, adds the triangle run
  and a "Details" summary where there is none, and gives a gap the
  visibility of the block after it; the Mac folds before rendering and
  before the side page's split (`MCFoldedRuns`), so a click changes the
  folded runs and the side page rebuilds just those pieces. State is
  `MCTab detailsOpen` by tag line; the summary's link is
  `mutible-details:<tag line>:<run line>:<open>`, the run line giving a
  kept side piece's shift (`toggleMarkdownDetails:atIndex:inPage:`).
  Quotes carry `quoteDepth` and, inside a list item, `quoteIndent`; a quote
  keeps its own list stack (`quoteList`), and a fence in an item its depth.
  An ordered list counts on from its first item's number (2026-11-18,
  `md:list-numbers`, CommonMark and GitHub: "1. 3. 4." shows 1, 2, 3):
  `analyze()` keeps a `Numbering` per open list level (and per level of
  a quote's own lists; a nested quote's items keep their numbers), which
  a block out of the level, a sibling of the enclosing item, or another
  delimiter or bullet ends. The marker run carries the counted number,
  so the GTK and Android pages follow; the HTML export already wrote
  `<ol start>` with the first number and let the browser count.
  Between blocks comes exactly one `"\n"`
  run with `gap` set, however many blank lines. Brackets are matched once per
  block with a stack and nesting stops at 8, so bad input stays linear.
  `MarkdownEdit` asks `MarkdownParser::lines` rather than classifying itself,
  so the preview and a double-click agree. Tests: `md:*` (GFM's in
  `tests/markdown_gfm_tests.inc`), `tests/markdown/corpus.md`, and the
  `markdown_preview` GUI suite.
- **GitHub's alerts, emoji and front matter** (2026-11-19, round 9's
  "render plainly"; `md:alerts`, `md:emoji`, `md:front-matter`,
  `markdown_preview`'s corpus step). Alerts: the quote's first line
  `[!NOTE]` (TIP, IMPORTANT, WARNING, CAUTION, any case, an outer quote
  only) becomes its title paragraph; `analyze()` keeps the kind per quote
  group and every block of the group gets `Block::alert`, stamped on
  `MdRun::alert` (the title's runs `alertTitle`); the gap runs inside take
  the block's after them, so the bar is not cut. The Mac colors the outer
  bar by kind (`MCAlertColor`, GitHub's light and dark shades), sets the
  title in it after an SF Symbol tinted to it (`MCAlertIcon`), the text in
  the body color, and marks the runs `kMarkdownAlert` with the kind's
  name; a quote block remembers its kind (`kMCAlertKey`) so two quotes
  never share a bar. MarkdownHtml writes GitHub's markup
  (`markdown-alert markdown-alert-<kind>`, `markdown-alert-title` with a
  small inline SVG) and both shades in its stylesheet. Emoji:
  `MarkdownEmoji` (core, about 900 of gemoji's 1,800 names, one code point each, U+FE0F
  added where the default is text) is applied at the end of `inlineRuns`
  to text runs only (not code, math, pictures, footnote numbers, or an
  address shown as itself), so a table measures after it; a code needs
  no letter or digit outside its colons ("12:100:30" stays). Front matter
  (the parser's old rule: `---` on line 0, a `key:` on line 1, `---` or
  `...` within 200 lines) was a code block; it is a `Block::Table` of one
  row of keys over one of values (`frontMatterRows`: lists, nested maps and
  block scalars joined in the cell, quotes dropped, punctuation but `( )`
  escaped, since `\(` opens math), its runs `frontMatter`. Its lines stay
  `MdLine::Code` of one edit block, so a double-click edits the YAML as
  code: the Mac leaves `kMarkdownTableCell` off that table, and the
  corpus's editable check reads its line, not a cell. Linux and Android
  show the table and the emoji as they are, and an alert as a quote with a
  bold title until they read `alert`.
- **Export as HTML…** (File menu, no key; 2026-11-08): `MarkdownHtml` (core,
  `md:html-export`) writes a standalone page from the same runs: GitHub's
  heading anchors, lists rebuilt from `listDepth`/`quoteIndent`/`quoteDepth`
  (a `gap` outside a quote ends the quotes open), footnotes in a `<section>`,
  math as `\(...\)` text. Links keep only relative, #, http(s) and mailto
  addresses, pictures relative, https and data:image, the scheme read as a
  browser reads it (leading and trailing controls and spaces stripped, tabs
  and line breaks removed, and again with references decoded and every
  control dropped; every reading must pass): `&#9;javascript:` was a
  javascript: link to the browser. The golden page is
  `tests/markdown/corpus.html`: after a deliberate change,
  `MC_WRITE_GOLDEN=1 ./build/run_tests` writes it again; read the diff.
  The Mac writes it through the atomic writer after a save panel, or where
  `MCHtmlExportPathChooser` says (the harness).
- **The Mac's page** (+MarkdownRender): a paragraph's style comes from its
  first run; quotes and code blocks are `NSTextBlock`s, one object carried
  across a block's paragraphs so its bar or box is drawn once (a quote one
  block a level, nested); in a list item the bar or box sits in a plain
  indent block of its own, kept on it as an associated object, since a
  margin on the box itself still had its background drawn under it; h1, h2 and rules
  are bordered text blocks; list text at 24 points a level with the marker
  right-aligned on a tab; a hard break is U+2028; gaps are 12 points.
  **The measure** (2026-11-08, `markdown.max-width`, 840 points at 100%,
  about GitHub's README width; VS Code's markdown.css has no limit): lines
  are at most that times `MCZoomScale`, the page centered by the text
  view's horizontal inset (`PageMeasure::centeredInset`), so text blocks
  and tables, which are container-wide, keep to it. The full preview sets
  it in `applyMarkdownMeasure` (+MarkdownRender: on render, on the text
  view's frame change, from `applyWordWrap` and `setPlainMessage`, which
  put the editor's 8 back for source); the side pane in `applyMeasure`,
  asked by `pageWidth` before every page. Fenced
  code naming a language (`MdRun::lang`,
  `MarkdownParser::extensionForLanguage`) is colored by `MarkdownCode.mm` after
  the page is built. Tables: runs carry table, row, column and alignment, and
  the Mac builds an `NSTextTable` (`appendMarkdownTable:`); a table holding
  typeset formulas gets `MCTableColumnWidths` (AppKit's table layout ignores
  attachment widths). Cells are padded by display width, never bytes.
- **A page draws through `MCGapLayoutManager holdsPage`** (2026-11-09, the
  owner's screenshots; `make guitest SUITES=markdown_page`; the full
  preview, the side pane, the split editor's copy). AppKit's
  `glyphRangeForBoundingRect:` assumes line fragments only go down, and a
  table's go back up at each cell, so a strip inside a row of uneven cells
  (each piece that scrolls in) found no glyphs and stayed blank; the
  override adds the rows the rect touches, by each row's top (those do go
  down) and only up to what is laid out. `fillBackgroundRectArray:` rebuilds
  a wrapped run's background from each line's glyphs (AppKit joins whole
  lines and runs to the fragment's edges); **`rects` is the layout manager's
  own buffer, overwritten by the next rect it answers, so copy it first**,
  and only a run's own `NSBackgroundColorAttributeName` goes this way:
  selections, text blocks and table borders come through the same method.
  A page replaced while scrolled down redraws the whole pane
  (`MCRedrawWholePane`): TextKit marks only the page's own column there, so
  a new zoom's margins kept the old page's ink. **The same on a tab
  switch** (2026-11-16, the owner's screenshot: settings.conf's text, its
  swatches too, in the margins beside JOBS.md's page, the title saying
  [Preview]): a page shown in place of another tab's text marked only its
  column. `CodeTextView redrawWholePane` marks the whole view (the
  visible and prepared rects until 2026-11-18, see the next trap) now
  and again from a run-loop block, where the pane ended up;
  `activateTab:` and `renderMarkdown:` call it, and the view calls it when
  shown again (`viewDidUnhide`: marks made while hidden had no visible
  rect). markdown_side records the view's marks across switches.
  **Trap: mark the whole view, not what shows** (2026-11-18, the owner's
  screenshot of `.grades/STATUS.md` on 1.6.3, a file a script rewrites
  every few minutes, in a 2300-pixel window: an older, narrower copy of
  the page in its left margin, cut off where the page's column starts,
  starting partway down). CodeTextView's `setNeedsDisplay:` override and
  `redrawWholePane` marked only the visible and prepared rects, so
  NSTextView's own whole-view marks (a new inset as the pane widens)
  reached no further, while the layer keeps drawn tiles past them and
  TextKit's marks for the new layout cover the page's whole column at
  every height: those tiles came back with a fresh column and the old
  margins. Both now mark `bounds` with `avoidAdditionalLayout:YES` (no
  layout, as TextKit's own column marks; only what is drawn costs), and
  `applyMarkdownMeasure` calls `redrawWholePane` whenever the inset
  moves. `setNeedsDisplay:` still marks what shows with layout first: an
  inset that changes during layout (a legacy scroll bar appearing as a
  page goes in) stops `ensureLayoutForTextContainer` short, and that mark
  laid the screen out again (without it markdown_preview read a picture's
  size as -1 by -1). A tab read again from disk while its page shows
  (`refreshDisplayKeepingPage`, from `takeDiskRead:`, `takeDiskText:` and
  `putBackText:`) is rendered at the same height down the pane, clamped
  to the new end; it went back to the top on every reload until then.
  `markdown_page` rewrites a long page in place (truncate and write, as
  Python's `open(..., 'w')`) and by renaming over it, scrolled near its
  end at 2300 points, resizes the window and the sidebar between, and
  checks the place, one text view over the slot, the centered measure,
  and a mark holding the whole view (saving pictures under
  `MC_GUI_SNAPSHOTS`).
- Every run carries its source `line` (`kMarkdownSourceLine`): Shift+Cmd+V
  opens the preview at the caret and the source where a scrolled preview is.
  Links (`openMarkdownLink:`): `#heading` by GitHub's anchor
  (`MarkdownParser::anchor`, as on Linux and Android), web addresses in the browser, paths relative to the
  file through `openTerminalLink:` (`#L12` works).
- **The preview to the side** (Ctrl+Cmd+V, `markdown.showPreviewToSide`;
  2026-11-07; no longer a `markdown` language command since 2026-11-19,
  when LaTeX and HTML files got a page to the side too, in
  `EditorController+PageSide.mm` (latex.md, viewers.md); the item is off
  for any other file, in the controller's and the app delegate's
  `validateMenuItem:` alike, since round 9 found it on and doing nothing
  for an HTML file): `MarkdownSidePreview.{h,mm}` is
  the window's pane (its own read-only TextKit 1 view, the bar, its share,
  the 0.3 s re-render timer); `MCTab sidePreview` says whether a tab wants
  it (in the session as `sidePreview` paths). **At scale** (2026-11-08):
  the page is built in pieces (`MarkdownPage`, core, `md:page-*`: split at
  line starts outside quotes, code and tables, where the builder's state
  starts over) with the full preview's builder in three steps
  (`beginMarkdownPageWidth:`, `markdownRuns:from:to:anchors:`,
  `markdownNoteForRuns:`; `markdownPageFor:` is them over one piece). After
  a pause the parse, the split and `MarkdownPage::match` against the page
  on show run on a queue (`renderSidePreviewAfterTyping`); on the main
  thread only unmatched pieces, and pieces still waiting for a picture,
  formula or diagram (`keep`), are built and spliced in, a storage edit per
  stretch, never the whole text (`renderSidePreviewFrom:keepingPlace:`,
  `spliceSidePage:`). The pane's runs carry `MCMarkdownLine` objects, one a
  line of a piece, so a kept piece's lines move without touching the text
  (read them with `MCMarkdownLineOf`, which takes the full preview's
  NSNumbers too; a task box's line comes from its run, not its link). A
  page of another context (path, width, zoom, theme, math, Mermaid, web
  pictures; `sidePreviewApplySettings` drops it) is built whole. A click on
  a page older than the buffer (`renderedSource`) renders first and acts on
  nothing (`catchUpSidePage`). `lastRenderCpuMilliseconds` is what the
  suite bounds (under 10 ms at 1,200 lines, 30 at 20,000).
  The pane steps aside while its tab shows the full preview, so the two
  pages never show at once and share the per-render state (`_awaitedMath`,
  `_awaitedWebImages`, the Mermaid page); arrivals go through
  `renderShownMarkdownKeepingPlace`. Layout: `layoutSidePreviewIn:textShown:`
  splits the text's rect in `relayoutRightArea`.
  **Scroll sync** (W25, 2026-11-07, VS Code's scrollPreviewWithEditor,
  scrollEditorWithPreview and markEditorSelection, no settings): the pane
  observes the editor's clip view, its own, and the editor's selection
  (`followSource:...`, lines from the gutter's `LineIndex`). The map is
  `PreviewScroll` (core, `previewscroll:*`): a mark per source line's first
  run, interpolated top to next top both ways, so the two maps are
  inverses. The marks' lines and characters come from the runs; a mark's
  top is found only when asked (`topOfMark:`, laying out that far), and a
  question needs two marks (`PreviewScroll::around`), so the page is never
  laid out whole unless the source's end is on show (`pageBottom`). Trap:
  `setNeedsDisplayInRect:` on the pane after a splice laid out the page to
  that rect in small steps, each walking every laid-out line fragment (the
  text blocks' slow path, see the editor's trap): 4.3 s at 20,000 lines;
  the mark uses `avoidAdditionalLayout:YES`. Only the user's moves lead:
  the pane's own (render, resize,
  `scrollY`, following) go through `holdFollowing:`, and a bounds change
  that leaves a side's y as last seen is ignored. After a render
  `pageRendered` follows the source unless `pageLeads` (the user scrolled
  the page last; typing in the source clears it). A double-click in the
  pane is the block editor (`editMarkdownIn:atCharacter:point:`), its edit
  one `replaceRanges:` splice in the source (`applySidePreviewEdit:`).
- **Editing from the preview**: a double-click (`onPreviewDoubleClick`) reads
  the line (and `kMarkdownTableCell`), `MarkdownEdit::blockAt` finds the block
  (heading text, list item with continuations, paragraph or quote, code between
  fences, a table cell with newlines and pipes escaped back, a `$$` block
  whole); `applyMarkdownSource:` splices, marks dirty, registers undo,
  re-renders at the same scroll. An edit made while the buffer changed
  underneath (an agent's rewrite read in, Cmd+Z) is made on the text as
  it is then, the block found again by `MarkdownEdit::rebaseSpan`
  (`MCMarkdownBlockNow`, `mdedit:rebase`; round 10's documents defect 3,
  2026-11-19); when the block's own text changed, or is there more than
  once, it is not applied: its text goes to the clipboard with a status
  note (`MCKeepLostMarkdownEdit`). `markdown_page` rewrites the file
  under an open box both ways. Trap: never splice a box's edit at the
  offsets it opened with into text that changed since.
  The box opens on the double-clicked word (2026-11-15, D2's first step):
  `MarkdownEdit::sourceOffsetFor` finds the page's word in the block's
  Markdown by the 40 characters either side (letters and digits compared,
  markup looked through, `mdedit:word`), and `MCMarkdownEditPanel
  showText:...selection:width:font:` selects just it, is as wide as the
  page's lines, in the page's 15 points for prose, and as tall as the
  text; nothing is preselected otherwise. The LaTeX popover selects the
  clicked word the same way (`MCLatexClickedWord`). Editing in place on
  the page, Typora's way, is the planned second step (D7; it needs a
  design).
  **The box keeps its draft** (2026-11-19, D2 on 1.6.3 and 1.6.4,
  `markdown_page`). Trap: a transient popover closes when the app loses
  the keyboard, and what was typed in it goes too. The popover is `NSPopoverBehaviorSemitransient`, so
  switching apps leaves it open (transient closed it, and the typing went
  with it); a click elsewhere in the window still closes it. Only Esc and
  Cancel (`close`) drop the draft; Return and Apply (`save:`) apply it;
  AppKit closing it any other way applies it (`popoverWillClose:`, unless
  `_settled`), and so do a tab leaving (`stashActiveTab`), a second
  double-click (`editMarkdownAtCharacter:` and
  `editSidePreviewAtCharacter:` apply first, the full page's character then
  read anew), Cmd+S, closing a tab, a window or quitting
  (`commitPageEdit`, before the unsaved-changes question). The button is
  Apply and the title "Return applies": it puts the edit in the buffer,
  never on disk (the owner's call: Apply, since auto-save may write it).
  In two editor groups the other group shows a page as a picture
  (`MCMirrorPictureView`, which accepts the first mouse); a two-click press
  there (its first click spent making the window key) focuses the group
  and opens the box on the page now under the pointer
  (`otherGroupPressed:` then `editPageAtWindowPoint:`), and a one-click
  press focuses it so the second click lands on the page itself.
  Offscreen the popover never shows, so `markdown_page` checks the
  behavior, posts the app's resign and become-active notifications, and
  calls `popoverWillClose:` by hand.
  **GTK** (2026-11-19, linux/HANDOFF.md item 120, `edit_boxes`): the
  block editor says "Enter applies, Shift+Enter for a new line" and has
  Apply; it is not autohide (`PopoverKeep.h`: an autohide popover closes
  on an app switch, on X11 because the click on the other program is its
  grab's outside click), so an app switch keeps it and its draft; a press
  elsewhere in the window applies the draft, as do `Editor::stash` (a tab
  leaving), the page turned to its source, Ctrl+S (matched in the box's own
  key controller from win.save's accelerators: a key typed in a popover
  never reaches the window's), Save All, closing a tab
  or the window and quitting (`Editor::commitPageEdit`, before any
  question); Escape and Cancel drop it. The other group shows no rendered page
  on GTK (it names the file until clicked, `Groups.cpp`), so the Mac's
  two-group double-click change has no counterpart.
- **Pictures**: `MCMarkdownImage` attachments with `MCAnimatedImageCell` (sized
  to the line; a GIF's timer runs only while on screen). An `<img>`'s width
  and height (`MdRun::width`/`height`, `MdLength`: CSS pixels or a percent of
  the line, read as HTML reads dimensions, `md:img-size`) go to
  `initWithPicture:size:widthPercent:`, pixels times `MCZoomed`; one side
  keeps the proportions, and anything wider than the line is scaled down
  whole. A percent height is ignored, as a browser does with an auto-height
  page. Traps: the TextKit 1
  editor never asks for a TextKit 2 view provider (1.4.1 shipped still GIFs);
  `attachmentCell` set in init reads back nil, so it is an override. **Web
  pictures** (`MCWebImageLoader`): https only (redirects too), one ephemeral
  NSURLSession (no cookies, credentials or cache; 15 s idle, 30 s total;
  `User-Agent: Mutible/<version>`), `data:image/` decoded in place,
  `markdown.web-images = false` off. The render never waits: alt text stands in
  (`kMarkdownWebImage`), the address joins `_awaitedWebImages`, and the
  notification queues one re-render 0.1 s later through
  `renderMarkdownKeepingPlace` (the top character kept by subtracting what
  arrived above). Limits: 20 MB, 50 megapixels; an NSCache of bytes, not images
  (the animated cell sets frames on its own image); failures remembered 30 s;
  nothing on disk. NSImage draws SVG; an SVG's `<image href>` is never fetched.
  Checked without the web: `markdown_preview` gives the loader's session (made
  at its first fetch from `ephemeralSessionConfiguration`, swapped for the
  step) a URL protocol answering `https://pictures.mutible-test.invalid/`
  half a second late, in both previews, and sees http never asked for.
- **Video and audio** (2026-11-15, `MarkdownMedia.{h,mm}`, `md:media`):
  `MdRun::media` (1 video, 2 audio) from `<video>`/`<audio>` (src, else the
  first `<source src>`; what else is inside is a browser's fallback and is
  hidden) or a picture whose address has a media extension
  (`MarkdownParser::mediaKind`). `MCMarkdownMedia` is the attachment: its
  cell draws the first frame and a play button, or an audio bar, from
  `MCMediaInfo` (playable, size with rotation, poster, worked out once per
  URL on AVFoundation's queue; https assets send no cookies), and lays
  itself out again when that arrives (it finds its own character). A click
  (`trackMouse:`, or `playInView:frame:` for checks) puts an AVPlayerView
  over it as a subview of the page's text view, which the cell's drawing
  keeps in place; scrolled out of view it pauses; `stopMedia` (tab change,
  window close) calls `MCMarkdownMediaStopAll`; a replaced page frees the
  attachment, whose dealloc takes the player away. Kept side pieces keep
  their attachment, so typing does not stop a playing video; a full
  preview render (a formula or picture arriving) does. The GUI checks make
  their own clip with AVAssetWriter (guitest links CoreVideo for it).
- **Task lists** (all ports): `MdRun::task`; the Mac's box is an `MCTaskBox`
  with `NSLinkAttributeName` `mutible-task:<line>` (`clickedOnCell:` never
  fired), applied as a preview edit, never saved by itself (the user ruled out
  autosaving preview edits); a double-click's second click on a box is
  swallowed. Done text takes `markdown.done`. Toggle Task is Option+Cmd+L
  (Ctrl+L, leader L); TODOs Shift+Cmd+D (Ctrl+Shift+L, leader W). "[ ] item"
  without a dash is not a task (GitHub, Obsidian, VS Code agree), and the task
  key adds only the dash. The TODOs window is Find in Folder's in a mode; a tag
  counts after a comment opener or first on a line with a colon, and in
  Markdown only after `<!--`. A numbered task's marker is "3. ☐": the
  number stands before the box on every port.
- Empty Markdown and LaTeX files open in the source on every port.

## Math in the Markdown preview (Mac)

Typeset by tectonic; elsewhere (Linux, Android, no tectonic, `markdown.math =
false`) a formula shows as its TeX in the code style.

- **Parser**: read before other inline rules. Inline `$...$` as pandoc (opener
  not before a space; closer not after a space nor before a digit, so "$5 and
  $10" stays text); a backslash pair and a code span are skipped inside.
  `$$...$$` inline is display, `\(...\)` inline. Display blocks run from a line
  starting `$$` or `\[` to one ending `$$` or `\]` without a blank line,
  interrupting a paragraph; plus ```` ```math ````. `\[...\]` counts only as a
  block (inline it is an escaped bracket). Memos keep unclosed openers linear.
  `tests/markdown/math.md` has every case.
- **The batch** (`MathTex`): each formula in
  `\setbox\mcbox\hbox\mcbgroup\aftergroup\mcafter$...$\mcegroup\mcsentinel` (never
  a macro argument, so TeX recovers from a stray `\par`), shipped by `\mcship`
  as a page of its box plus 1 pt; the log's `MC:<nonce>:BEGIN:k` and
  `MC:<nonce>:k:page:wd:ht:dp` give pages, boxes and errors (a fresh nonce
  per run, so a formula's own `\typeout` is no box); the output routine drops
  other pages. `check` refuses what could derail later formulas (unbalanced
  braces, a stray `$`, `\input`, `\end{document}`, `\write`, `\stop`) and the
  ways round those words (`\csname`, `\catcode`, `\scantokens`,
  `\makeatletter`, `\ExplSyntaxOn`, any `^^`), and what would outlive the
  formula's group into later ones, whose pictures are cached under their own
  keys (2026-11-07): `\global`, `\gdef`, `\xdef`, `\globaldefs`, counters,
  `\aftergroup`, `\afterassignment`, the group words, `\fontdimen` and the
  other always-global assignments, and the prefixes `\mc`, `\XeTeX`,
  `\Declare`, `\New`, `\Renew`, `\Provide`, `\AddTo`, `\AtBegin`, `\AtEnd`;
  and (2026-11-08, `kVersion` 4) page-level state and file reads:
  `\special` and `\pdf...` in every form, `\pagecolor` and the other
  `\page...` words (its background special fills every later page),
  XeTeX's `\filedump`/`\filesize`/`\mdfivesum`, `\font` and the font
  selectors, `\IfFileExists`, `\RequirePackage`. **Trap**: `\begin{x}` runs
  `\csname x\endcsname` and `\end{x}` `\csname endx\endcsname`, so
  `\begin{pagecolor}` was `\pagecolor` past the word list; an environment's
  name must be letters in braces and is checked as both words, ignoring
  case, and `\lowercase`, `\uppercase`, `\Make...`, `\UseName` and
  `\ExpandArgs` are refused as more ways to build a word. **Trap**
  (round 10's documents defect 1, `kVersion` 5): what `prepare` drops
  (`\label{...}`, an equation environment's `\begin` and `\end`) joined
  the letters either side into a word the check never saw
  (`\glo\label{q}bal` is `\global`), and a Greek letter spelled as a
  bare Latin one did the same (`\catc`, omicron, `de`); `check` now also
  checks `prepare`'s output (inline and display), and `spellUnicode`
  braces a letter it spells (`{o}`, `{T}`). A
  formula that still closes its box early (`{$}`: TeX closes a brace for the
  `$`) has its rest gobbled unread to `\mcsentinel` by `\mcafter`; one that
  leaves a group open makes `\mcship` (built only from `\mc` copies of the
  primitives) skip the rest of the batch, so the run has no PDF and the others
  go again. **Trap**: `\globaldefs=-1` around each formula broke every
  formula's fonts (LaTeX loads fonts lazily with global definitions); check a
  change to the batch with a real tectonic (`markdown_math` with
  `MC_GUI_REAL_TECTONIC`). `prepare`
  rewrites MathJax-only forms (`\mathbb{1}` to dsfont's `\mathds`, align and
  friends inline, `\tag`, Unicode symbols); the preamble and macros are in
  MathTex.cpp. **Bump `MathTex::kVersion`** when any of it changes: it is in
  every cache key.
- **The run** (`MarkdownMath.mm`): requests gather 50 ms into one run on a
  serial queue; `-Z continue-on-errors --untrusted --print --keep-logs --reruns
  0` (`--untrusted` keeps shell escape off). A bad formula fails alone with
  TeX's complaint as its tooltip; a page must be the size the log gave; a run
  with no PDF fails the formula TeX was in and reruns the rest. A watchdog
  kills tectonic after 15 s without output once in the formulas (the last
  `MC:BEGIN` names the culprit), 60 s before (downloads). A run reaching no
  formula fails them for 60 s with the reason at the page's top (`firstError`
  reads tectonic's panic when the bundle is out of reach). Failures stay in
  memory.
- **Cache**: whole PDFs under `~/Library/Caches/Mutible/math`
  (`$MUTIBLE_MATH_CACHE`) with a `.idx`; formulas of a run share fonts (a PDF
  each was far bigger). Key: SHA-256 of `kVersion`, display or inline, and the
  TeX (pages stay vector, so no size). Past 50 MB, down to 40 MB.
- **Drawing** (`MCMathAttachment`): vectors filled with the text color in
  source-in mode, so a formula takes the color around it; the frame stretched
  to the font's ascender and descender (an attachment-only line sat high) with
  margins drawn past it; a formula wider than a line scaled to the whole line's
  width, not what is left of it. Trap: an attachment glyph's location is the
  bottom of its frame, not the baseline.
- **Never blocking**: the cache's formulas show at once, the rest as TeX
  (`kMarkdownMath`), re-rendered on `MCMathTypesetNotification` keeping the
  place. While formulas are out the top line says "Typesetting N formulas…",
  and that a first run downloads LaTeX files when tectonic's cache is empty (a
  first run once sat 44 s with only TeX showing). Since 2026-11-15 the run
  counts tectonic's "note: downloading " lines as it reads them
  (`MCMathProgressNotification`, `MCMathFilesFetched`, at most five a
  second) and the line says "N of about 290" (`MCMathFirstRunFiles`,
  measured with 0.17.0 and an empty cache), changed in place in the full
  preview (anchors shifted) and by a render keeping place beside the
  source; `MCDownloadTectonic`'s success calls `MCMathWarmUp`, one formula
  typeset when tectonic's cache is empty, so the files come at once. Without tectonic, a muted
  line with a Download tectonic link (`MCDownloadTectonic`, shared with the
  LaTeX preview).
- Known gaps: copying a typeset formula copies an attachment character;
  `\color` inside is drawn in the text's color.

### Mermaid in the preview (Mac)

```mermaid blocks go to the user's mermaid-cli (`mmdc`, MCFindProgram, or
`$MUTIBLE_MMDC`), one run at a time off the main thread, each its own
process group with 90 s (a run starts a headless browser, about 2.5 s):
a transparent PNG at scale 2 in the preview's theme (dark or default),
kept in memory and `~/Library/Caches/Mutible/mermaid` (`$MUTIBLE_MERMAID_CACHE`;
50 MB). Chosen over a renderer of our own (flowchart and sequence alone are
a layout engine) and over mermaid.js (third-party code). PNG, not PDF or
SVG: mmdc 12 makes PDFs fit by default and refuses the older `--pdfFit`,
draws PDF backgrounds white, and its SVG labels are HTML in foreignObject,
which NSImage leaves out. Until it arrives, and without mmdc or with
`markdown.mermaid = false`, the block is code (a failure's complaint, from
`MermaidDiagram::errorSummary`, its tooltip); the code carries
`MCMarkdownMermaidAttribute` so `renderMarkdownKeepingPlace` keeps the
place. Core: `MermaidDiagram.{h,cpp}` (`mermaid:*`); Mac: `MarkdownMermaid.mm`,
a hook in `renderMarkdown:`. `make guitest SUITES=mermaid` uses a stand-in
mmdc; `MC_GUI_REAL_MMDC=<an mmdc>` adds a real diagram and a picture.

**mermaid.js without mmdc** (2026-11-15, the owner's call, change 25):
without mmdc a line above the page's first diagram (`downloadNoteForRuns:`)
offers Download Mermaid (`MCMermaidDownloadLink`, `downloadMermaidForPreview`):
one pinned `mermaid.min.js` (`MermaidDiagram::kMermaidJsVersion`, 12.3.0,
from jsDelivr's npm mirror, `kMermaidJsSha256` checked against the npm
tarball's file) into Application Support, refused unless its SHA-256 matches
(`$MUTIBLE_MERMAID_DOWNLOAD` and `$MUTIBLE_MERMAID_SHA256` for checks;
`$MUTIBLE_MERMAID_JS` uses a file as it is), and checked again once a
launch. `MCMermaidWeb` draws: one WKWebView never in a window, a
non-persistent data store, the script as a user script, a content rule list
blocking every subresource and a navigation delegate refusing every load
but its empty page (`MCMermaidWebLoadsRefused`), `callAsyncJavaScript` with
`MermaidDiagram::renderScript()` (Mermaid's own labels, after
`document.fonts.ready`; the SVG put in the page at its viewBox's size and
its place returned), then `createPDFWithConfiguration:` of that place, the
view drawing no background (`drawsBackground` NO by KVC). **Trap
(2026-11-16, round 8 D3)**: the SVG was handed to NSImage until then, whose
SVG drawing ignores the `<style>` Mermaid puts every color in and `em`
offsets on tspans: labels sat above their boxes, text and boxes were dark
grey, and each curved edge, its `fill: none` lost, filled as a black wedge;
never draw Mermaid's SVG with NSImage. The key holds the drawer
(`kDrawerMmdc`, `kDrawerMermaidJs`, `kVersion` 3), the PDF is cached as
`<key>.pdf` (vectors, sharp at any zoom), mmdc wins when installed, and the
side page's context holds `MCMermaidDrawerState()`. The `mermaid` suite
plays a stand-in mermaid.js whose colors come from its style sheet.

## Traps

Each trap's first sentence, in the order it appears above; the whole
text is in place in its section.

- Markdown: **Trap: mark the whole view, not what shows** (2026-11-18, the owner's screenshot of `.grades/STATUS.md` on 1.6.3, a file a script rewrites every few minutes, in a 2300-pixel window: an older, narrower copy of the page in its left margin, cut off where the page's column starts, starting partway down).
- Markdown: Trap: `setNeedsDisplayInRect:` on the pane after a splice laid out the page to that rect in small steps, each walking every laid-out line fragment (the text blocks' slow path, see the editor's trap): 4.3 s at 20,000 lines; the mark uses `avoidAdditionalLayout:YES`.
- Markdown: Trap: a transient popover closes when the app loses the keyboard, and what was typed in it goes too.
- Markdown: Traps: the TextKit 1 editor never asks for a TextKit 2 view provider (1.4.1 shipped still GIFs);
- Math in the Markdown preview (Mac): **Trap**: `\begin{x}` runs `\csname x\endcsname` and `\end{x}` `\csname endx\endcsname`, so `\begin{pagecolor}` was `\pagecolor` past the word list; an environment's name must be letters in braces and is checked as both words, ignoring case, and `\lowercase`, `\uppercase`, `\Make...`, `\UseName` and `\ExpandArgs` are refused as more ways to build a word.
- Markdown: Trap: never splice a box's edit at the offsets it opened with into text that changed since.
- Math in the Markdown preview (Mac): **Trap** (round 10's documents defect 1, `kVersion` 5): what `prepare` drops (`\label{...}`, an equation environment's `\begin` and `\end`) joined the letters either side into a word the check never saw (`\glo\label{q}bal` is `\global`), and a Greek letter spelled as a bare Latin one did the same (`\catc`, omicron, `de`); `check` now also checks `prepare`'s output (inline and display), and `spellUnicode` braces a letter it spells (`{o}`, `{T}`).
- Math in the Markdown preview (Mac): **Trap**: `\globaldefs=-1` around each formula broke every formula's fonts (LaTeX loads fonts lazily with global definitions); check a change to the batch with a real tectonic (`markdown_math` with `MC_GUI_REAL_TECTONIC`).
- Math in the Markdown preview (Mac): Trap: an attachment glyph's location is the bottom of its frame, not the baseline.
- Mermaid in the preview (Mac): **Trap (2026-11-16, round 8 D3)**: the SVG was handed to NSImage until then, whose SVG drawing ignores the `<style>` Mermaid puts every color in and `em` offsets on tspans: labels sat above their boxes, text and boxes were dark grey, and each curved edge, its `fill: none` lost, filled as a black wedge; never draw Mermaid's SVG with NSImage.
