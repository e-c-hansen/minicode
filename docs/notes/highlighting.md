# Highlighting

Area notes moved out of `CLAUDE.md` (see its "Area notes" list). The text
below is as it stood there; section names quoted in it ("see ...") are
found through that list.

## Highlighting

- `SyntaxHighlighter` lexes a line at a time from a `LexState`;
  `IncrementalHighlighter<Ch>` keeps line starts and end states and re-lexes
  from an edit until a line ends in its old state. States over 16 bits (a
  raw-string delimiter, a template stack) keep their text in a process-wide
  table (`stateTextId`), so equality stays two integer compares. Tokens start
  and end at ASCII characters, so UTF-8 and UTF-16 agree.
- Lines end after '\n', a '\r' not before '\n', and U+2029 (three bytes in
  UTF-8), as `LineIndex` counts (`endsLineAfter`); `lexAnyLine` shows the
  lexers a '\n' in a foreign break's place, so they still test for '\n'
  only. Whether a line starts at s depends on units [s - reach, s]
  (`breakReach`: 1 in UTF-16, 3 in UTF-8), so `edit` drops and finds again
  the starts in that window on both sides of an edit. A line over
  `kMaxTokenizationLineLength` (20,000 units, VS Code's
  editor.maxTokenizationLineLength) is never lexed or even read: no tokens,
  the state carries over; bracket folding skips its brackets.
- Grammars worth knowing: Python f- and t-strings (fields lexed as code; a
  triple-quoted or continued f-string carries `0x100` in its quote; a field
  open at a line's end is not carried); `lexJsLine` for JS and TS (regex
  literals only where an expression can start, `kJsBeforeRegex`, never after a
  name, number, string, `) ] }` or `<`, closed on their line; template literals
  across lines through a stack); `lexLongString` (C++ raw strings, Rust raw
  strings, Go backticks, triple quotes in Java, Swift, Dart, Groovy, Kotlin and
  Scala, C# verbatim and raw); Rust lifetimes are not strings; `lexTexLine` (a
  blank line ends open math, so an unclosed `$` colors only its paragraph). Not
  done: Swift `#"..."#` and `\(...)`, Kotlin/Scala/Dart/C# templates, Groovy
  slashy strings, a block comment across lines inside a JS template field, a
  regex at line start continuing a division.
- Styles past the first eight (2026-11-08): `Escape` (a string's `\n`,
  `\x41`, `\u{...}`, split out by `pushText`; none in raw strings, a
  shell's or YAML's '...', regexes; Python's raw prefix rides in the
  state as `kRawPrefix`) and `Self` (self, cls, this, super by language);
  a name after def/fn/func/function/fun is Function and after
  class/struct/enum/union/interface/trait a Type (`wordBefore`, same line
  only); a Python decorator is `@name.attr` in Function, its arguments
  code. Anything asking "inside a string?" asks `isStringStyle` (Escape
  counts); arrays by style are `kTokenStyleCount` long on every port.
- **HTML and XML** (2026-11-15, `lexMarkupLine`, `tests/markup_syntax_tests.inc`,
  `syntax:markup*`, the `previews` suite's last steps on a .vue and an .xml file): three styles of their own, `Tag`, `Attribute` and
  `TagBracket` (`syntax.tag`, `syntax.attribute`, `syntax.tag-bracket`,
  each theme's entity.name.tag, entity.other.attribute-name and
  punctuation.definition.tag); values are String, entities Escape. Lines
  end in `LexState::Markup` (inside a tag, a value, a comment or CDATA,
  the `kMu*` bits) or, in HTML and Vue, `MarkupEmbed` inside a `<script>`
  or `<style>` body: the state text of the body's kind and the state the
  JavaScript or CSS line lexer (run on the slice before the first
  `</script`/`</style`, which ends the body even in a string, as browsers
  do) ended in. A script whose type is not JavaScript or JSON is plain.
  XML (svg, plist, ...) has no bodies. Folding asks `openToken(state)`,
  never the kind: a body line is code unless its JavaScript left a string
  or comment open. A new token style needs its color in every theme row,
  `kSyntaxNames`, the default file, Linux's `Palette.h` tag names (a
  missing one made a second "plain" tag) and Android's `Palette.styles`
  (indexed by style).
- Tests: `tests/LegacyHighlighter.h` is the frozen old lexer, an oracle under
  random edits; where it cannot apply (`newGrammar`, `mayHoldFString`,
  `mayHoldNewLiteral`, `hasOtherBreak`: it split at '\n' only) the edits
  compare against a full lex. Elsewhere `fullLex` holds the line lexer to it
  through `legacyView` (escapes joined back into their strings, every name
  classified by the old rules; Python text with an @ is `mayHoldDecorator`,
  a full lex), and the incremental highlighter to the full lex.
- The Mac (+Highlighting): `applyHighlighting` is the full pass (and
  `recolorEditor` on settings); `textStorage:willProcessEditing:` feeds edits
  to the highlighter and records lines; `textDidChange:` (or a zero-delay
  perform when there is no `didChangeText`) calls `flushHighlighting`. Up to
  64K characters, and at any length without tokens, `restyleChangedIn:` sets
  attributes only on pieces whose run differs (each merge or split memmoves
  the run array; setting a 5 MB line's attributes again made TextKit lay it
  out again). Traps: never change
  attributes inside `willProcessEditing` (it widens the edit and the caret
  jumps to its end); reset a range with one `setAttributes:`, never
  `addAttribute:` over many runs (3.4 s for 50k lines against 1 ms).
- **Semantic tokens** (Mac, 2026-11-07, `editor.semantic-highlighting`):
  `LspSession (Semantic)` asks `/full` (then `/full/delta`) on show, 0.3 s
  after typing and on the server's refresh, one request at a time, and hands
  `Lsp::SemanticSpan`s (decoded in the core against the text the server
  answered for) to `semanticTokensArrived:` only while `_edits` is
  unchanged. +Highlighting keeps them while the characters are unchanged
  (any character edit ends them, in `willProcessEditing`, before its
  `_liveHighlight` check) and paints only lines on screen
  (`paintVisibleLines`, from the clip view's bounds notification, a
  `_semanticPainted` index set), through `restyleChangedIn:` with the spans
  over the lexer's tokens (never over strings or comments): the same
  diff-only painting, so no new attribute traps. An edit's own lines get the
  lexer's colors at once; other lines keep painted colors, which move with
  their text, until the next answer. Theme colors (`Theme::semantic`) are
  VS Code's for the scopes each token type probes; font styles are left out.
- **Bracket-pair colors** (Mac, 2026-11-08, `editor.bracket-pair-colorization`,
  `BracketPairs.{h,cpp}`, `bracketpairs:*`, `make guitest
  SUITES=bracket_pairs`): VS Code's, all kinds counted together, three
  colors by depth and `syntax.bracket-unexpected` for a closer at depth 0
  (a closer of the wrong kind is not unexpected here). Depth is never in
  the lexer state: each line has a `LineSum` (net, floor; lines compose),
  kept in `Depths` blocks of about 512 lines, updated in
  `willProcessEditing` from the tokens `_hl->edit` re-lexed. Colors are
  overlays in `restyleChangedIn:` like the semantic spans (never over a
  string, escape or comment); `_bracketPainted` holds the characters
  whose brackets are current. An edit that changes the depth below it
  empties the set from the edit on; `paintVisibleLines` (the old
  semantic painter, now both) repaints the lines on screen, for brackets
  alone only their bracket characters (so the server's moved colors
  stay), and the rest as they scroll in (or come on screen as the clip
  view or text view changes size; a run-loop block, so before the window
  draws). An open, a theme or the zoom paints only the lines on screen
  too (every bracket at once doubled an open's CPU). The set follows
  edits through `BracketPairs::PaintedEdits`, applied once at the flush:
  moving it per edit was quadratic in a multi-range edit. Off with no
  highlighter (large files, plain text) and in the settings file.
- **Spell checking** (Mac, 2026-11-16, `editor.spell-check`: on, off,
  prose-only by default; `editor.spell-check-color`; core
  `SpellCheck.{h,cpp}`, `spell:*`; Mac `SpellMarks.{h,mm}`, an
  `MCSpellMarks` per CodeTextView like the folds; `make guitest
  SUITES=spell_check`). The core picks the words: Markdown's prose by
  `MarkdownParser::lines` (from the text's top, since a fence above
  decides) less code spans, `$` math, links' addresses and tags; LaTeX's
  and code's from the highlighter's tokens (LaTeX: everything but
  commands, their `[...]` options, the groups of naming commands such as
  `\label`, `\cite`, `\usepackage`, math and verbatim; code: comments, and
  strings that read as sentences); then only words of letters (no digit,
  `_`, `/`, inner dot, camelCase or two capitals). Only those go to
  NSSpellChecker, copied into a string of spaces as long as the lines on
  screen, so the answer's ranges are the text's
  (`requestCheckingOfString:`, off the main thread, the words picked on a
  queue of their own). Checked 0.5 s after typing stops (one pending
  perform, not one a key) and as lines scroll in; an edit only moves the
  marks after it and drops those it touches. The +Highlighting hook
  (`updateSpellCheck`, from applyHighlighting) gives the kind and the
  lines' tokens (`spellLinesFor:tokens:`). Ignore is per document tag (one
  per path, for the text view's life). **NSTextView's continuous checking
  never runs in the editor**: CodeTextView's setters keep it off and its
  menu drops AppKit's Spelling and Grammar submenu (a copy of the menu,
  so a menu AppKit keeps never gathers items); it would mark code and lay
  out the whole text. The commit box uses the system's own continuous
  checking (small, all prose), off with `editor.spell-check = off`.
- Linux retags in idle slices over a UTF-8 mirror; Android's `Highlighter.kt`
  uses the core over a native mirror.

## Traps

Each trap's first sentence, in the order it appears above; the whole
text is in place in its section.

- Highlighting: Traps: never change attributes inside `willProcessEditing` (it widens the edit and the caret jumps to its end); reset a range with one `setAttributes:`, never `addAttribute:` over many runs (3.4 s for 50k lines against 1 ms).
