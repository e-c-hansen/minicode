# The session's host on the Mac (editor.session-host)

SESSION-DESIGN.md (in `.grades/`) is the design; its "Stage 4a as built"
and "Stage 4b as built" say what was built. GTK's side is
`linux/HANDOFF.md` item 87. This file is the Mac's.

## What it is

With `editor.session-host = on` (off by default for one release), or
`MINICODE_SESSION_HOST=on` in the environment (the GUI checks' second
pass), every tab whose text is set after the setting is on gets a document
on the session's host: the core's `Shared::SessionHost` on a thread of its
own in the app's process, one `Shared::Client` in UTF-16 units for the
whole process (`mac/src/SessionLink.{h,mm}`). The host owns that
document's text and version; the tab's text is its mirror. A tab opened
before the setting changed keeps the way it was opened until it closes.

- `MCTab session` (an `MCSessionDoc`) is made by `MCTab setSourceText:`
  the first time the tab's text is set with the setting on, and closed
  with the tab (`dealloc`).
- **The storage's edits.** `MCSourceStorage willReplaceCharacters` is
  called before each character replacement, while the store still holds
  the old text. `EditorController+Session.mm` installs it once
  (`installSessionHooks`): while `_liveHighlight` is on (the storage holds
  the active tab's source), the edit goes to the active tab's document as
  an operation (`storageWillReplace:with:view:`), widened to whole code
  points so a surrogate pair is never split, and the tab is made to follow
  the storage at once (`followStorage:`), so its `currentText` is never a
  copy from before the edit.
- **Texts set whole** (a CSV table's edit, a preview's, a background tab
  following the disk, a pending sort): `MCTab setSourceText:` sends the
  one range in which the old and new texts differ.
- **Read afresh** (`forgetHistoryOf:`, a reload by choice, a first read):
  the host's copy is reset to the tab's text, its history gone, and any
  text set whole in the same run-loop turn resets it again rather than
  being an edit.
- **Steps.** The edits of one run-loop turn are one step, as
  NSUndoManager's grouping by event made them: it opens at the first edit
  and closes on the main queue's next turn. Its name is the first given:
  `replaceRanges:withStrings:actionName:`'s name (`beginNamed:view:`, before
  the storage edits), or a later `setActionName:` on the tab's undo manager
  (`MCTabUndoManager`); with none, "Typing" when the edit came from
  `insertText:` or a delete or insert command (`CodeTextView
  sessionTyping`), which joins the step before while the caret stays put,
  else "Edit". `breakUndoCoalescing` and a selection set by anything but an
  edit (`noteSelection:`) start the next typing step anew.
- **The language server sync** (`Lsp+Sync.mm`, `hostText`): the document
  on show is watched through the client (`Client::watch`, a copy of the
  host's text kept from the host's own edits), settled before each flush,
  and read for `didChange` and `didOpen`; the storage's edits still feed
  the `ChangeTracker`. A host text of another length than the storage's
  (an edit of the host's waiting to go in) is sent whole. The watch is let
  go when the document leaves the view and at shutdown.
- **Saving** tells the host (`MCSessionDoc saved`) when the text written is
  the tab's; dirty, the disk, backups and auto-save stay the Mac's (stage
  7 moves them).

## The host's edits

They come back through `MCSessionDoc hostEdited:` (the client's listener):
an undo's or redo's (once the host owns undo) and, with a second screen,
anyone else's. Into the storage through `replaceRanges:withStrings:
actionName:` as one multi-range edit when the operations are replacements
apart in the text (`Ot::asReplacements`), else one operation at a time,
with `applying` set so nothing is recorded; into a tab not in the storage
as its `sourceText`, then `tabTextChangedByHost:` (dirty, title, backups).

**Re-entrancy.** The client's waits (`settle`, `undo`, `undoInfo`, a first
`watch`) dispatch every message that arrived, wherever they are called:
the language server flushes before each request, and completion asks from
inside `NSTextStorageDidProcessEditing`. An edit of the host's that
arrives while the storage is editing (`editedMask` set, inside
`processEditing` and its notifications) is never put in there: it waits
(`deferredEdits`) for the next run-loop turn, and an edit of the mirror
made meanwhile is moved past it before it is recorded (`Ot::transform`),
and it past that edit. The normal dispatch is a main-queue block at the
top of a turn, never inside a text system callback.

## Checks

- `make test`: `host:*`, `ot:*` (the core), `host:watch-timeout` (a watch
  the host does not answer in time keeps no watcher, and its late answer
  is dropped), `settings:*` for the key.
- `make guitest`: at every suite's end the harness settles with the host
  and fails the suite (`FAIL [suite] the session's host and the window it
  renders differ`) unless every tab with a document has the host's text,
  nothing of the host's waits to go in, and the host reported nothing
  wrong (`CheckSessionMirrors`, `MCSessionCheckTabs`). With the setting off
  no tab has a document and the check does nothing.
- `MINICODE_HOST=loopback` puts every message through the protocol's
  encoding and back; `MINICODE_HOST_DELAY_US` delays each one on the host's
  thread. Errors the host reports go to the log (`os_log`, subsystem
  com.minicode.editor, category session).

## Traps

- **Never edit the storage from the hook.** `willReplaceCharacters` runs
  inside the storage's edit: it records and sends a message, nothing else
  (no layout, no other edit, no wait).
- **`_liveHighlight` decides** whether a storage edit is the active tab's
  source. It is off whenever the storage holds something else (a preview,
  a message), as CLAUDE.md's rule already says; an edit made with it off
  while the storage holds source would go unrecorded, and the mirror
  check fails the suite that made it.
- **A whole fill is never an edit.** `displaySourceEditable` and the
  large-file emptying in `makeEditorReadOnly` turn `_liveHighlight` off
  first; showing a tab again puts its own text in, which the host already
  has.
- **UTF-8 cannot carry a lone surrogate.** Opening and resetting a
  document send UTF-8 (`doc.open`, `doc.reset`); a text holding one (none
  read from a file: such a file goes to Quick Look) becomes U+FFFD in the
  host's copy, and the mirror check would say so.
- The host keeps a UTF-16 copy and its saved text, and the language server
  sync a third copy: about 150 MB more for a 25 MB file. Large-file mode
  has no server, so two.
