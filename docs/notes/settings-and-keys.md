# Settings, themes and keys

Area notes moved out of `CLAUDE.md` (see its "Area notes" list). The text
below is as it stood there; section names quoted in it ("see ...") are
found through that list.

## Settings, themes and Adjust Settings

- `AppSettings` loads `~/.config/mutible/settings.conf` (or
  `$MUTIBLE_SETTINGS`), watches it, and posts the change notification. The
  command values of `lsp.*`, `format.*` and `run.*` are whole command lines up
  to a ` #` comment outside quotes, unlike other keys.
- **Swatches**: in the settings file each color value is a swatch
  (`NSLinkAttributeName` `mutible-color`); a click opens NSColorPanel and
  `colorPicked:` rewrites the line with `Settings::setColor` (uncommenting it),
  saving after 150 ms. An opacity value is a `mutible-opacity` link opening
  `MCOpacityPopover` (`Settings::setOpacity`, keeping a `%` form).
  `linkTextAttributes` is only a pointing-hand cursor. Trap: an NSTextView with
  `usesFontPanel` sets the shared NSColorPanel to the color under the caret
  when the selection moves, and the panel forwarded that to `colorPicked:`, so
  every pick became the text color; the editor sets `usesFontPanel = NO`.
  The panel is opened through `MCShowSwatchColorPanel` (Appearance.h), whose
  local monitor makes Esc close it (and the opacity popover) while the
  keyboard is in the panel or the settings file; the panel never takes the
  keyboard by itself, so the key lands in the file. Closing it clears its
  target, so later changes of the shared panel are no picks.
- **Adjust Settings…** ends every surface's context menu and opens the file at
  that surface's keys (`Settings::appearanceSurfaces()`: `tree`, `git`,
  `editor`, `syntax`, `markdown`, `terminal`, `browser`, `statusbar`,
  `titlebar`, `window`, placeholders `tabs` and `terminal-tabs`; own keys, then
  the `window.*` fallbacks). A new view adds it with
  `MCAddAdjustSettingsItem(menu, @"surface", self)` at the end of
  `menuForEvent:` (`willOpenMenu:withEvent:` for WKWebView) or `view.menu =
  MCAdjustSettingsMenu(...)`; `MCSurfaceTextField` does it for a field's
  editing menu (the field editor asks the field, never its delegate). The keys'
  lines are tinted with temporary attributes for about 3 s. A file older than
  some keys gets their commented defaults (`Settings::missingSurfaceLines`)
  appended under one "# Added by Adjust Settings" line, one undo step, saved at
  once only if it had no unsaved changes. The ruler is left out of the map on
  purpose (old files would get new lines).
- **Themes**: `theme = <id>` (`kThemes`: dark-modern, dark-plus, monokai,
  solarized-dark, tomorrow-night-blue, light-modern, light-plus, quiet-light,
  solarized-light, mapped by hand from VS Code's files) gives every color; any
  color the file sets wins. Unset is dark-modern, VS Code's own default
  (2026-11-14; until then dark-plus, mutible's look from the start):
  `Settings::theme_` starts there, every color reads the theme, and
  `settings:default-theme` holds an empty file to `theme = dark-modern`
  color for color; `defaultFileText` shows Dark Modern's values, and
  `linux/src/Palette.h` and Android's `Palette` (Core.kt) follow them.
  Opacity is separate. View > Theme
  (`MCThemeSubmenu`) writes the line through `Settings::withTheme` (CRLF kept)
  and, when the file sets colors of its own (`colorOverrides`), offers to
  comment them out. `Settings::isDark()` sets the window's NSAppearance;
  popovers that force DarkAqua and inline hex are unchanged.
  `selectedTextAttributes` is only a background, since after a switch AppKit
  drew selected text in its own color.
- **mutible's own writes** to settings.conf and keys.conf (a theme, the IDE
  link toggle, the first file of either) go through `MCWriteConfigFile`
  (AppSettings.h, over `FolderSearch::writeFileAtomically`), never
  `writeToFile:atomically:`, which replaced a symlinked dotfile with a plain
  file. A theme or the IDE link is refused while a tab holds the file with
  unsaved changes (`AppSettings.unsavedTextCheck`, set in
  EditorController+Settings.mm's `+load`), the reason in the status bar.

## Keys: the registry, keys.conf and the key monitor

- `src/Commands.cpp` has every command (about 160) in menu order: id, title,
  menu, default keys (`shift+cmd+p` notation), where it applies (`Any`,
  `Editor`, `Terminal`, `Tree`), selector and tag, flags (`Global`: the key
  monitor runs it before any view, the grid included; `Fixed`: a view's own
  keys, listed and protected but not changeable yet). `resolve()` applies
  `keys.conf` (`~/.config/mutible/keys.conf` or `$MUTIBLE_KEYS`): `key = id`
  adds, `key = -` takes the key from every command, `key = -id` from one
  (an id from before the rename, `minicode.x`, names `mutible.x`: <!-- rename-mutible: keep -->
  `Commands::find` and `keysOf` take either, by `Rename::otherCommandId`); a key
  given to a command leaves any that had it where both apply; two lines giving
  one key to two commands in one place are a conflict and the later is refused
  with its line number. A command may also have a language (`language`,
  "markdown" or "python", by `Commands::languageOfPath`; its `when` is then
  `Editor`): it applies only in the editor on such a file, shadows a key
  another command has elsewhere without a conflict (Option+C toggles a task in
  Markdown and types ç anywhere else), may have a typing key (Shift+Return in
  Python), and its keys go on its menu item only when no other command has
  them and they hold Command or Control. The window's delegate says the
  language (`MCKeymapLanguage`). Cmd+B stays the sidebar and Cmd+I the inline
  agent in Markdown (the owner's call, 2026-11-06): Toggle Bold and Italic
  have no key.
- `Keymap.mm` reads and watches the file and puts the keys on the menu items
  found by action and tag (the first on the item, others on hidden items beside
  it, `allowsKeyEquivalentWhenHidden`). Errors join the settings errors in the
  status bar; the hints, the palette and mutible > Keyboard Shortcuts… (which
  writes the file, every command commented out, the first time) follow it.
- The key monitor asks `MCKeymapHandleKeyDown` first, which runs a command
  (validated as its menu item would be) when it is Global (Ctrl+` and
  Ctrl+Shift+`, the Menu key, Cmd+keypad 0, Option+] and [ during a tour); when
  the key has neither Command nor Control (Shift+Option+F and O, F2, F8, F9,
  Shift+F12) and the editor has the keyboard, since AppKit gives such a key to
  the text view as typing before the menu (in another field or the grid it
  types); when two commands share a key in different places; and for keys a
  menu cannot show (keypad digits, the Menu key, Ctrl+Shift with punctuation);
  and for a `Tree` command (renameFile on F2, moveFileToTrash on Cmd+Delete,
  mutible.tree.newFolder on Shift+Cmd+N, which is New Window elsewhere) while the tree has the keyboard; and for a function key whose `Any`
  command something in the responder chain takes (F7 in the compare view's
  panes, plain text views that would beep at it). A `Tree` command's keys are never put on
  its menu item, since a menu key acts from every pane (Cmd+Delete once
  trashed the tree's selection from the editor); no view hard-codes them.
  The grid's prompt line edits (Cmd+Left/Right/Delete, Option+Left/Right) and
  the tree's Space and Esc (Quick Look) are `Fixed` rows: typing keys, or
  keys the view sends on to the shell. A plain Control key goes to the terminal grid while it has the keyboard
  (Ctrl+G for nano) unless Global. Plain typing skips the keymap at once. Cmd+.
  is the key bindings' `cancelOperation:` everywhere, so only an editable
  editor sends it to the menu. Validation decides: a command that cannot act
  lets the key go (Option+] types ‘ with no tour). A language command runs
  from here too, first, where it applies; a typing key is looked up only
  while the editor has the keyboard.
- **The hints panel** (Shift+Cmd+H, `EditorController+StatusBar.mm`;
  2026-11-18). Its keys are symbols, from one core function,
  `Commands::keyLabel(key, Platform)`: on the Mac `glyphs()`, Apple's
  ⌃⌥⇧⌘ before ⏎ ⌫ ⌦ ⇥ ⎋ ↑ ↓ ← → (`MCKeymapGlyphs` calls it); on Linux the
  modifiers in words joined by "+", in GTK's and VS Code's order, and the
  same symbols for the keys that have one: "Ctrl+Shift+P", "Ctrl+⏎",
  "Shift+⇥", "⎋" (Home, End, Page Up, Page Down, Space and Menu keep their
  names; a Linux user reads ⇞ less readily than Page Up). Words for the
  modifiers on Linux, because Ctrl and Alt have no symbol a PC keyboard
  prints, and "Ctrl+⇧+P" mixes the two ways in one key. It is not
  `linuxLabel()`, which docs/REFERENCE.md's Linux column and the status bar
  keep. Menus draw their own keys. `HintsSearch::parse(query, Spelling)`
  finds keys by name in either spelling ("return" ⏎, "shift+ctrl+h"
  Ctrl+Shift+H, "control" Ctrl on Linux). The rows are read as one list,
  a pair's two shortcuts one after the other, and
  `PageMeasure::layoutHints(items, room, gap, columnGap, maxColumns)` cuts
  it into as many columns as fit the room (16 at most): the tallest column
  as short as can be, then the heights as even as can be, a heading or the
  gap before "Hide these hints" kept with the item after it, each column's
  stops from its own widest key and label (`measure:hints`). The Mac draws
  every line of the text as one paragraph of uniform height
  (minimum = maximum line height) holding that line of each column, with
  that line's own tab stops, so a short label never sends a tab to the
  wrong column; a heading has a blank line above it except at a column's
  top. With no room for one column the text is the old one column, labels
  wrapping at their stop. `hintsText` (tests) is one column.
- **Trap (1.5.0 and 1.5.1)**: Shift+Option+F typed Ï, because the "verified"
  check had sent the event to the menu directly. Test such keys through
  `[window sendEvent:]` or `MCGuiSendKeyToApp`, never the menu alone.
- **The hints' search box on Linux** (2026-11-18, `linux/src/HintsSearchBox.{h,cpp}`,
  `linux/HANDOFF.md` item 108): the Mac's box (CLAUDE.md, "Its search box")
  as a GtkSearchEntry at the panel's top, matched by the core's
  `HintsSearch` with `Spelling::Linux` (the panel draws keys with
  `Commands::keyLabel(key, Platform::Linux)`, `Keys::hintLabel`: "Ctrl+Shift+H",
  "Ctrl+⏎", so "control", "opt", "return" and "shift+ctrl+h" find them). "Without Command or
  Control" is without Ctrl or Super there. main.cpp's key controller in the
  window's capture phase (`onHintsKey`) hands every key press to
  `HintsSearchBox::takeKey` while the panel shows and the palette is
  closed: it takes the keyboard at the first key (remembering where from,
  given back on close, the editor when a click put it there), forwards the
  key to the entry's GtkText (`gtk_event_controller_key_forward`), and types
  an Alt+letter's letter itself. Marks and dims are Pango attributes on the
  label (a background, a foreground alpha), so the text and its size never
  change. The controller, added to the window after GTK's own, runs before
  the application's accelerators on GTK 4.22: Alt+Z reached the box and
  left Word Wrap alone with nothing unbound (checked by taking Keys out of
  it; the accelerators of Ctrl keys and function keys run because the box
  lets those go). **Trap**: an input method types nothing for Alt+letter
  (`GDK_MODIFIER_INTENT_NO_TEXT_INPUT`), so a forwarded Alt+Z was lost
  (hints_panel's "both are the box's" failed with "s"); the box inserts
  the letter itself.

## Traps

Each trap's first sentence, in the order it appears above; the whole
text is in place in its section.

- Settings, themes and Adjust Settings: Trap: an NSTextView with `usesFontPanel` sets the shared NSColorPanel to the color under the caret when the selection moves, and the panel forwarded that to `colorPicked:`, so every pick became the text color; the editor sets `usesFontPanel = NO`.
- Keys: the registry, keys.conf and the key monitor: **Trap (1.5.0 and 1.5.1)**: Shift+Option+F typed Ï, because the "verified" check had sent the event to the menu directly.
- Keys: the registry, keys.conf and the key monitor: **Trap** (the hints' search box on Linux): an input method types nothing for Alt+letter (`GDK_MODIFIER_INTENT_NO_TEXT_INPUT`), so a forwarded Alt+Z was lost.
