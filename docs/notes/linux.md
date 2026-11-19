# The Linux port

Area notes moved out of `CLAUDE.md` (see its "Area notes" list). The text
below is as it stood there; section names quoted in it ("see ...") are
found through that list.

## The Linux port

`linux/PARITY.md` is the plan for catching up with the Mac, and its list says
what is done; start there. `linux/HANDOFF.md` (items 1 to 14) and `BUILD-LINUX.md` (build, and what is
unverified). It shares the highlighter, Markdown, tasks, settings, comments,
AutoIndent, KeepSorted, FolderSearch, TextFind, LatexDoc, SyncTex, Json,
LspClient, TermLinks, GitStatus, GitGraph and GitModel; VTE is its terminal. The core
lists in `linux/meson.build` and `linux/Makefile` must agree
(`linux:build-lists`; CI builds only with Meson). Its traps are in HANDOFF.md:
accelerators taking keys from VTE (`Commands::linuxTerminalTakes`, once `updateShellKeys`; VTE's cursor colors, item 110), the selection color
(`ThemeCss.cpp`), one click and autoselect in the tree, `settleOnCaret`,
`<Ctrl>greater`, `ScrollSettle.h`, links acted on from an idle, embedded
widgets off the view before a re-render, `Terminal::topRow`, an Alt+letter
forwarded to a field typing nothing (the hints' search box, item 108;
`docs/notes/settings-and-keys.md`), two submenus of one label in a popover
menu, whose second button opens the first's page (bookmarks, item 112),
a list's rows remade under the keyboard, which leaves the window's focus
on the removed row until GTK's next frame, every key until then lost
(`ListKeyboard.h`; the Agents and Processes panels, item 110),
a tab strip's reveal waiting for its frame undoing a wheel turned before
it (`TabStrip::settle`, item 115),
an autohide popover closing, and its typing lost, when another program
takes the keyboard (GDK's grab on X11, the compositor on Wayland;
`PopoverKeep.h`, item 120).

## Traps

Each trap's first sentence, in the order it appears above; the whole
text is in place in its section.

- The Linux port: Its traps are in HANDOFF.md: accelerators taking keys from VTE (`Commands::linuxTerminalTakes`, once `updateShellKeys`; VTE's cursor colors, item 110), the selection color (`ThemeCss.cpp`), one click and autoselect in the tree, `settleOnCaret`, `<Ctrl>greater`, `ScrollSettle.h`, links acted on from an idle, embedded widgets off the view before a re-render, `Terminal::topRow`, an Alt+letter forwarded to a field typing nothing (the hints' search box, item 108; `docs/notes/settings-and-keys.md`), two submenus of one label in a popover menu, whose second button opens the first's page (bookmarks, item 112), a list's rows remade under the keyboard, which leaves the window's focus on the removed row until GTK's next frame, every key until then lost (`ListKeyboard.h`; the Agents and Processes panels, item 110), a tab strip's reveal waiting for its frame undoing a wheel turned before it (`TabStrip::settle`, item 115), an autohide popover closing, and its typing lost, when another program takes the keyboard (GDK's grab on X11, the compositor on Wayland; `PopoverKeep.h`, item 120).
