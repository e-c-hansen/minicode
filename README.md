# minicode

minicode is a 1.7 MB editor with a terminal, a browser, a file tree, and a Markdown and LaTeX preview in one window, all driven from the keyboard. It's written from scratch in C++, with no Electron and no third-party dependencies, and runs on macOS, Linux, and Android.

![Clicking through the demo folder in minicode](docs/demos/tour.gif)

```
brew install --cask e-c-hansen/tap/minicode
```

The [Linux](#linux) and [Android](#android) builds are further down.

I built it because I wanted one light place to browse a folder, edit code with a language server behind it, preview Markdown and LaTeX, and keep a terminal and a browser a keystroke away, without a few hundred megabytes of runtime underneath.

The macOS app links only against frameworks that ship with the system. The Linux version is a GTK4 port and the Android version is a Kotlin app; all three share the same C++ core. The rest of this README describes the macOS app, and the Linux and Android sections say how the others differ.

## What it does

Open a folder and you get a file tree on the left and the file you click in the main pane. Each file you open gets a tab above the editor, keeping its own unsaved changes, undo history, and place, and a dot on the tab means it has changes to save. Typing something and deleting it again leaves nothing to save. Command W closes the tab, or the terminal or browser when it has the keyboard, and Shift Command W closes the window. The next time you open the folder, its tabs come back. On the Mac, unsaved changes are also copied to a backup a second after you stop typing, so if minicode crashes, is force quit, or the Mac loses power, the next window on that folder brings them back as unsaved tabs, with the file on disk untouched and a note if it changed in the meantime. Quitting still asks whether to save, and Don't Save throws the backups away. The tree follows changes made elsewhere, in the terminal, in git or in another program, and right clicking an item offers new file, new folder, rename, move to trash, reveal in Finder, and copy path or the path from the folder. On the Mac, right clicking a tab offers the same for its file, plus showing it in the tree and closing the tab, the others, the ones to its right, or all of them.

On the Mac, Shift Command P opens a command palette, as in VS Code: type a few letters of any command in the menus and press Return to run it, with the ones you ran last at the top. Command P opens the same box on the folder's files instead, matched by name with the files you opened lately first, and Control G asks for a line of the open file (42, or 42:7 for a column). Typing > at the start of the box switches to commands, and a colon to a line number.

On the Mac the file tree can show up to three columns at the right of each row, each switched on and off by its own key: Shift Command I for sizes, Option Shift Command I for when each file was last saved, and Control Shift Command I for when it was created. They use Finder's units ("4 KB", "2 min", "yesterday", "12 Oct"), with the exact figures in the row's tooltip, and when the row gets narrow the rightmost column goes first. A folder's size is the total of everything inside it, hidden files included, as Finder and du count it; it is added up in the background, shows "…" until it is known, and is counted again when something inside changes. A very large folder stops being counted after a couple of seconds and shows "≥" before what it reached. While the last-saved column shows, the status bar also says when the open file was last saved. Sort By in the tree's right click menu orders the tree by name, date modified, size, or date created, ascending or descending, with folders first or mixed in with the files. Dates and sizes start newest and largest first, as in Finder, and each folder remembers its own order. Shift F10 or Control Return opens the right click menu of whatever has the keyboard, at the selected row or the cursor, so all of this works without the mouse.

Source files are highlighted by extension: Python, the C family, JavaScript, TypeScript, JSON, shell, LaTeX and a few others. Markdown renders as formatted text, with working links, tables and images, and Shift Command V flips to the source. Pictures with an https address, such as the badges at the top of a README, are downloaded in the background and kept in memory only; markdown.web-images = false in the settings turns that off. Math written as on GitHub, $x^2$ inline and $$ or a math code block for a display, is typeset by tectonic (the LaTeX preview's engine, below) in the background and kept in a cache, so a page opened again shows its formulas at once; markdown.math = false shows them as their TeX instead. You can also edit the rendered page: double click a paragraph, heading, list item or table cell, and a small editor opens holding its Markdown. Images and PDFs open in the same pane, and reload when the file changes on disk. On the Mac so do video and audio files, such as mp4, mov, mp3, wav and flac, in the system's own player; nothing plays until you press Shift Command Space or the play button.

![Opening a PNG and then a PDF from the file tree](docs/demos/files.gif)

Return indents the way the language expects. In Python a line ending in a colon starts an indented block, the line after return, pass, break, continue, or raise goes back a level, and a new line inside an open bracket lines up with whatever follows the bracket, or is indented one level when the bracket ends its line. The line after a call spread over several lines goes back to where the call started, not to its last argument. In C, JavaScript, Go, Rust, and the other brace languages, Return after an opening brace indents, and in YAML so does Return after a key that ends in a colon. Return between a pair of brackets, as in `foo()`, puts the closing bracket on a line of its own. Typing the colon of else, elif, except, or finally moves the line back under its if or try, and a closing brace typed on an empty line moves back under the line that opened it. How far to indent comes from the file, whether tabs or two or four spaces: Tab inserts that much, Backspace in leading spaces removes that much, and Tab and Shift Tab on a selection indent and outdent its lines.

F9 sorts the selected lines. Lines between a `keep-sorted start` and a `keep-sorted end` comment, as used by Google's [keep-sorted](https://github.com/google/keep-sorted), are sorted the way that tool sorts them, its options included: F9 with the cursor inside such a block sorts it, Shift F9 sorts every block in the file, and editor.keep-sorted-on-save = true in the settings sorts them all whenever the file is saved. A start without an end leaves the file alone and the status bar says which line it is on.

On the Mac, editor.auto-save = after-delay in the settings saves each file a second after you stop typing (editor.auto-save-delay sets how long, in milliseconds), and editor.auto-save = focus-change saves when the editor loses the keyboard, to another pane, tab, window, or app. That keeps minicode and Claude Code, or anything else editing the same files, from tripping over each other: a file with no unsaved changes follows the disk quietly, keeping your place in it, and an auto-save never writes over a file that changed on disk since minicode read it. It names the file in the status bar instead, and Command S asks whether to keep your version or reload. Auto-saves don't run the formatter or sort keep-sorted blocks; Command S still does.

On the Mac, Command F opens a find bar at the top right of the editor, as in VS Code. It finds as you type, highlights every match, and says which one you are on ("3 of 17"). Return and Shift Return, or Command G and Shift Command G, go to the next and previous one, and Escape closes it. The three buttons beside the query, or Option Command C, W and R, match case, match whole words only, and read the query as a regular expression in JavaScript's syntax, which VS Code uses too. Option Command F adds a second field for the replacement, where $1, $2 and so on stand for a regular expression's groups. Return there replaces the selected match and moves on, and Option Command Return replaces them all in one undo step. Command E takes the selected text as the query. Shift Command F searches the whole folder with the same three options, and Option Shift Command F does so with a replacement: Replace All counts the matches first and asks ("Replace 42 matches in 7 files?"), and a result's right click menu replaces only that match or only that file. A file open in a tab is changed there, unsaved and undoable, as rename changes it; other files are written to disk whole, so nothing is left half written; and a file whose tab has unsaved changes is left alone and named.

On the Mac the editor has VS Code's keys for several cursors. Command D selects the word at the cursor, and each press after that adds the next place the word occurs as a whole word, going round to the top of the file at the end; with text already selected, it adds the next place that text occurs. Shift Command L selects every place at once, and Option Command Down and Up add a cursor on the line below or above. Typing, Backspace, Paste, and the arrow keys then work at every cursor, each edit one step to undo, and Escape goes back to one. Command L selects the cursor's line, and each press after it adds the next line; Control Shift K deletes the line, or every line a selection touches (Shift Command K stays Request a Change). With nothing selected, Command C and Command X copy or cut the whole line, and Command V puts such a line back above the cursor's line rather than in the middle of it, as VS Code does. A thin line down the editor at column 100 shows where lines get long: editor.ruler in the settings moves it, adds more (80,120), or turns it off, and editor.ruler-color sets its color.

Markdown task lists make a TODO list, written the way GitHub, Obsidian, and VS Code read them: `- [ ] buy milk` is a task still to do and `- [x] buy milk` one that is done. In the preview each task has a box you click (or tap, on Android) to tick or clear it. The file then has unsaved changes, as after any edit made in the preview, until you save it. Done tasks are greyed out and struck through (markdown.done in the settings sets their color), and the title bar says how many of the file's tasks are done. In the source, Option Command L (Ctrl L on Linux) turns the line, or every selected line, into a task, ticks it if it already is one, and clears it if it was ticked. A line written as `[ ] buy milk`, without the dash, is not a task to GitHub or to minicode and shows as plain text, so select such lines and press Option Command L once: each gets its dash and keeps its box. Return at the end of a list item starts the next item, with an empty box after a task and the next number after a numbered one, and Return on an empty item ends the list. Shift Command D (Ctrl Shift L on Linux, the leader key and then W on Android) lists every TODO, FIXME, HACK, XXX, and BUG comment in the folder, along with the open tasks in its Markdown files, and opens the one you pick at its line.

Control backtick opens a terminal at the bottom. It runs one persistent zsh with your own startup files, so aliases, history and state carry over from one command to the next, and colors come through. On the Mac it is a real terminal: zsh's own line editor runs in it, so your prompt, Tab completion, Control R history search, autosuggestions, fzf's key bindings and whatever else your .zshrc sets up work as they do in Terminal.app, and vim, less, htop and ssh draw on the same screen. Output scrolls up into 10,000 lines of scrollback (terminal.scrollback changes that), which the trackpad, the mouse wheel and Shift Page Up and Page Down page through, and Command Home and Command End jump to the top and the bottom; typing takes you back down. Command F finds text in it, scrollback included, highlighting every match, and Command K clears it. At the prompt, Command Left and Command Right go to the start and end of the line and Command Delete deletes back to its start, as in VS Code and iTerm. Drag to select, double click a word or a path, triple click a line, and Command C and Command V copy and paste. Option works as Meta for the shell's Alt keys, and terminal.option-as-meta = false lets it type the characters your keyboard layout puts on it instead. Shift Return sends what Claude Code takes as a new line rather than Enter. The older design, with the output as a log and an input line under it, is still there for a release with terminal.mode = line.

![The terminal panel running ls, git log and a Python script](docs/demos/terminal.gif)

On the Mac you can have several terminals, each its own shell. Control Shift backtick opens another in a new tab, Command backslash splits the one you are in so two sit side by side, and Option Command ] and [ move between them. Tabs you are not looking at keep running. Each tab is titled with the command running in it or the folder it is in, and Control Shift W, or the × on its tab, closes a terminal; closing the last one hides the panel. A terminal with something still running in it, a server, vim, or Claude Code, asks before it closes, and so do closing the window and quitting. Holding Command W down closes one thing, not everything in turn.

Shift Command Return runs the open file in the terminal. It saves the file, then types the command into the terminal on show as if you had typed it, so it stays in the shell's history and Up brings it back. A Python file runs with the python in the project's .venv, venv or env folder when there is one, and python3 otherwise; shell scripts, Go, JavaScript and Ruby have commands too, and run.<language> in the settings changes them or adds others. If that terminal is busy with something else, the file runs in a new one instead, and run.terminal = new gives every run a terminal of its own.

![Writing a short list in vim inside the terminal panel, then paging through git log](docs/demos/vim.gif)

Paths and URLs in the output are links. Command click src/main.cpp:42:7 from a compiler, or a file Claude Code mentions, and it opens at that line; a URL opens in the browser panel, which Shift Command B toggles. On the Mac the browser sits to the right of the editor rather than in its place, with a bar between them you can drag, and the terminal stays across the bottom under both. Any of the three can be hidden on its own (Shift Command E for the editor), and the others take its space. Command 1, 2 and 3 move the keyboard to the editor, the terminal and the browser.

![Command clicking a grep result to open it at its line, then a URL that opens in the browser panel](docs/demos/links.gif)

Control Shift G swaps the file tree for a small source control panel, on the Mac, on Linux and on Android (there the leader key and then V, in the file list's place). It shows the branch, how far it is ahead of or behind its upstream, and the changed files in two lists, staged and not yet staged, each with a one letter status. Arrow keys move through the files, Return shows the selected file's diff in the main pane, Space stages or unstages it, and Command Return (Ctrl Enter on Linux) in the message box commits. On the phone a tap on a file shows its diff, the plus or minus beside it stages or unstages it, and the Commit button, or the leader key and then Enter, commits; git runs in Termux there, so the project has to be in the phone's shared storage. Git does the work underneath, and when it refuses something, such as a commit with nothing staged, its own message appears in the panel.

Under the changes is the commit graph: the history of the current branch and its upstream, drawn in colored lanes the way VS Code's graph and `git log --graph` draw them, with branch, remote and tag labels on their commits. Commits you have not pushed yet are tinted blue with a hollow dot and an up arrow, commits on the upstream you have not pulled are tinted green with a down arrow, and a line above the graph says how many of each there are, or that the branch has no upstream. A checkbox shows every local and remote branch instead. The graph loads 200 commits at a time, with a row at the bottom for the next 200. Tab moves between the changes, the graph and the message box, the Down arrow goes from the last changed file into the graph, and Return or a click on a commit shows its author, date, message and diff in the main pane. The panel only reads history; checking out, branching, merging, pulling and pushing are left to the terminal.

On the Mac, Shift Command K (Request a Change to minicode, in the minicode menu) changes minicode itself. A bar opens across the top of the window, marked in amber with a DEV tag so it is never mistaken for an ordinary field, and names the source folder Claude will work in. Describe the change in its box, which wraps your text and grows with it (Shift Return starts a new line), and press Return, and a terminal tab opens in minicode's source folder running [Claude Code](https://docs.anthropic.com/en/docs/claude-code) with your request, so you can follow it, answer its questions, and approve its edits right there. When Claude has rebuilt the app with make, the bar offers to restart into the new build. The restart asks nothing about unsaved files: their backups come along, and the new build reopens every window's folder with its tabs, unsaved changes included, and continues the conversation with Claude in a terminal, so a follow-up request is one more message away. The source is the folder this minicode was built in; a copy installed from Homebrew has none, so the first request clones the release you are running into ~/Library/Application Support/MiniCode/source, in the same terminal, before Claude starts (this needs git, from Apple's command line tools). develop.source in the settings points it at a clone of your own instead, and develop.command sets the command, claude by default. Nothing is committed or released unless you ask Claude to.

Command I calls up the inline agent, as it does for VS Code's inline chat, wherever the keyboard is in minicode (Show Hover Info moved to Option Command I to make room). It asks [Claude Code](https://docs.anthropic.com/en/docs/claude-code), so that needs to be installed and signed in, and what it does depends on where you are:

- In a file, a bar opens under the cursor's line. Ask for a change ("add type hints", "handle the empty list") and Claude's edit goes straight into the file, on the selection if there is one, with the changed lines tinted. Return keeps it and Escape puts the text back; it is also one Command Z. Ask again in the same bar and the next change builds on the last. A question gets its answer in the bar instead, and the file is left alone. The agent sees the file as it is in the editor, unsaved changes and all, and may read the rest of the project, but it changes only this file and saves nothing. The Markdown preview works the same way, on the file's source.
- In a terminal, it writes the command you describe and types it at the prompt, for you to read and run with Return. It sees the folder and the last lines on the screen, so "rerun that with verbose output" works.
- Anywhere else, the file tree, the browser, the source control panel, a picture or a PDF, it hands your request to Claude Code in a new terminal tab of the open folder, with a note of where you asked from (the selected file, the page's address), so you can follow along and answer its questions. Command Return does the same from the other two, with the selection or the terminal's last lines included.

The answers in the bar come from Claude Code's print mode, allowed to read the project but not to change anything; the edit is minicode's to make. agent.command in the settings is the command it runs, claude by default, with flags such as --model if you want them.

Restart minicode, in the minicode menu or the ↻ at the right end of the status bar, does the same restart on demand, as VS Code's Reload Window does: the same bundle opens again with every window's folder, tabs, unsaved changes and browser page, and a conversation with Claude from Request a Change goes on where it was. The browser panel comes back on the page it was showing, with its back and forward history and its scroll position, after a restart and after an ordinary quit too.

Shift Command H lists the shortcuts that apply right now.

## LaTeX

Open a .tex file and minicode typesets it and shows the PDF where the editor sits, again after every change. Shift Command V flips between the page and the source.

You can also edit from the page. Double click some text (a heading, a sentence, a table cell, a list entry, an equation) and a small editor opens holding the LaTeX behind it. Change it, press Return, and only those bytes of your document are replaced, so macros and packages minicode does not understand are left alone. Inside a list it also offers to add an entry. If minicode cannot tell which piece of source a word came from, it opens nothing rather than the wrong text. Nothing is written to disk until you press Command S, and Shift Command S exports the PDF.

![Editing a list entry by double clicking it in the typeset page](docs/demos/latex.gif)

Typesetting is done by [tectonic](https://tectonic-typesetting.github.io/). If it is not installed, the preview offers to download it (about 22 MB), or you can run brew install tectonic. The first document downloads the packages it needs, and after that it works offline.

## Language servers

With a language server installed, minicode gives you completion, errors as you type, hover and go to definition. It knows clangd (C, C++, Objective-C), basedpyright, pyright, pylsp or jedi-language-server (Python), gopls (Go), rust-analyzer (Rust) and typescript-language-server (JavaScript and TypeScript), and finds them on your PATH and in the usual Homebrew, npm (nvm, fnm and volta included), pipx, uv, Cargo and Go folders. minicode never installs one itself, but it tells you how when one is missing; see [Installing servers and formatters](#installing-servers-and-formatters).

Problems get a red or yellow squiggle, and the status bar counts them, for the file on show and for every open file. On the Mac, Shift Command M, or a click on those counts, lists every problem the servers report, grouped by file with errors first, between the editor and the terminal; Return opens the one you pick, and Escape goes back to the editor. F8 and Shift F8 go to the next or previous problem in the file, with its message shown at the cursor. Files a server checks without your opening them are listed too. Completion opens as you type, after a dot, an arrow or a double colon and on the second letter of any word, and Control Space opens it anywhere. On the Mac, Tab takes the selected item, and Return takes it too when you opened the list yourself, when it opened after a dot, an arrow or a double colon, or once you have moved through it with the arrow keys; a list that opened by itself on an ordinary word leaves Return a new line, as in VS Code and Zed. Beside the list, the selected item's signature and documentation show, fetched as you move through it. Taking an item that needs an import, such as Path from pathlib in Python or a function from another header in C++, adds the import or the #include the server suggests too, in the same undo step. Typing an opening parenthesis or a comma shows the call's signature above the line, with the argument you are on in bold. F12 or Command click jumps to a definition. On the Mac, resting the pointer on a name for a moment shows its type and documentation, with any problem reported there, as VS Code does; the pointer can move into that box to scroll a long docstring, and moving away, typing or Escape closes it. Command I shows the same for the name at the cursor. Also on the Mac, F2 renames the name at the cursor everywhere the server finds it, unsaved changes in other tabs included, since every open tab's text is kept open on its server: files that were not open get a tab, and every changed file is left unsaved for you to look over, with one undo step per file. Shift F12 lists every reference to the name, grouped by file, and Return opens the one you pick. Command period lists the server's quick fixes for the line, such as adding a missing import or semicolon, and Shift Option O organizes the file's imports.

The status bar shows the open file's server with a colored dot: green when it is running, yellow while it starts, red when it failed or crashed, and gray when there is none. Click it, or choose Language Servers in the minicode menu, to see every language's server, the command and program it runs, its process id, and why it stopped, with buttons to restart, stop and start it, and the command that installs a missing one. A server that crashes is started again after one, two and then four seconds, and after a fourth crash it waits for you. Restart Language Server in the same menu restarts the open file's.

For Python, basedpyright is the one to install, with `uv tool install basedpyright`, which brings its own copy of Node. The server reads your code with a Python interpreter, so it finds the packages installed there: by default the folder's own .venv, venv or env, then the active virtual environment. Select Python Interpreter in the minicode menu, or Change in the Language Servers popover, lists the project's environments, conda, pyenv, poetry, pipenv, uv and the Pythons on your PATH, and keeps the choice for that folder. python.interpreter in the settings file sets the default for folders without one.

![clangd flagging an error, completing a member, showing hover info and jumping to a definition](docs/demos/lsp.gif)

On the Mac a language can have a second server beside its main one. In a Python project that configures Ruff, with a ruff.toml, a .ruff.toml, or a [tool.ruff] table in pyproject.toml, minicode runs Ruff's own server (ruff server) beside basedpyright whenever ruff is installed (`uv tool install ruff`). basedpyright still answers completion, hover, definitions, references and rename; both servers' problems are underlined and listed together, each saying which server it came from, and Command period offers both servers' fixes in one list, Ruff's Fix all among them. Shift Option O organizes the file's imports, with Ruff's rules when Ruff runs. The status bar names both servers, and the Language Servers popover gives each its own card, with its state, restart, stop and start, and the install command when it is missing.

The settings file can pick a different server or turn one off (lsp.python = pylsp, lsp.go = off, lsp.enabled = false), and give a language a second server or none (lsp.python.also = ruff server, lsp.python.also = off).

## Formatting

Shift Option F formats the file with the formatter you have installed for its language, on macOS for now. If a project has its own configuration, such as a .clang-format, a [tool.pyink], [tool.black], or [tool.ruff] table in pyproject.toml, a .style.yapf, or a ruff.toml, that formatter runs with the project's style. Ruff's lint settings alone are enough, since a project that lints with Ruff nearly always formats with it too, unless the same folder also configures pyink, yapf, or Black. JavaScript and TypeScript projects set up for Prettier (a .prettierrc, a prettier.config.js, or a "prettier" key in package.json) or Biome (a biome.json) are formatted with that. Otherwise minicode follows Google's style guides: clang-format with Google's style for C, C++, Objective-C, Java, JavaScript, TypeScript, Protocol Buffers, and C#; pyink at 80 columns for Python, or yapf's google style when pyink is not installed; gofmt for Go; rustfmt for Rust; and shfmt with two-space indents for shell scripts. Python formatters in the project's .venv or venv are used first, and Prettier or Biome in its node_modules.

With some lines selected, clang-format, pyink, and yapf format only those lines. The change is one step to undo, the cursor stays on the code it was on, and a file the formatter cannot parse is left alone, with the formatter's complaint shown next to the cursor. When the formatter is not installed, the note next to the cursor says how to install it, as below.

The settings file can give a language its own command (format.python = yapf --style=google), turn one off (format.cpp = off), or format each time you save (format.on-save = true).

## Installing servers and formatters

minicode installs nothing itself, but it says what to install at the moment you need it, on the Mac for now. Open a Python file with no Python language server and a line over the editor says so, with an Install button. That line shows once per language each time minicode starts, its × closes it, and Don't Show Again keeps it away for that language; the status bar keeps offering the same help as long as the server is missing. Format Document does the same when the formatter is missing, and so does each missing server in the Language Servers popover.

The help shows the command to run, with a Copy button and a Run in Terminal button. Run in Terminal types the command into minicode's terminal and leaves it for you to read and press Return; it never runs anything on its own. minicode suggests the first command your Mac can run: with uv installed it suggests uv, without it pipx or npm, and with none of them it adds the step that installs uv. The other ways are listed under it. Once the program is installed, minicode notices when the command finishes in its terminal, or when you come back to the window, and starts the server without a restart.

| Language | Server | Install it with |
|---|---|---|
| Python | basedpyright | `uv tool install basedpyright`; or `pipx install basedpyright`, `npm install -g pyright`, or `pipx install python-lsp-server` |
| Python, beside basedpyright, in projects that configure Ruff | Ruff (ruff server) | `uv tool install ruff`, `pipx install ruff`, or `brew install ruff` |
| C, C++, and Objective-C | clangd | `xcode-select --install` (the command line tools), or `brew install llvm` |
| Go | gopls | `go install golang.org/x/tools/gopls@latest`, or `brew install gopls` |
| Rust | rust-analyzer | `rustup component add rust-analyzer`, or `brew install rust-analyzer` |
| JavaScript and TypeScript | typescript-language-server | `npm install -g typescript-language-server typescript`, or `brew install typescript-language-server` |

| Formatter | Formats | Install it with |
|---|---|---|
| pyink | Python, in Google's style | `uv tool install pyink`, or `pipx install pyink` |
| yapf | Python, when pyink is missing | `uv tool install yapf`, or `pipx install yapf` |
| Black | Python projects set up for it | `uv tool install black`, `pipx install black`, or `brew install black` |
| Ruff | Python projects set up for it | `uv tool install ruff`, `pipx install ruff`, or `brew install ruff` |
| clang-format | C, C++, Objective-C, Java, JavaScript, TypeScript, Protocol Buffers, and C# | `xcode-select --install`, or `brew install clang-format` |
| Prettier | JavaScript and TypeScript projects set up for it | `npm install -g prettier`, or `brew install prettier` |
| Biome | JavaScript and TypeScript projects set up for it | `npm install -g @biomejs/biome`, or `brew install biome` |
| gofmt | Go | `brew install go` |
| rustfmt | Rust | `rustup component add rustfmt` |
| shfmt | shell scripts | `brew install shfmt`, or `go install mvdan.cc/sh/v3/cmd/shfmt@latest` |

The installers come from Homebrew: `brew install uv`, `brew install pipx`, `brew install node` for npm, and `brew install go`. Rust's rustup comes from [rustup.rs](https://rustup.rs), and Homebrew itself from [brew.sh](https://brew.sh).

minicode looks for these programs on your PATH, in Homebrew's folders, in ~/.local/bin (where uv and pipx put them), in npm's global folder (nvm, fnm, and volta included), and in ~/go/bin and ~/.cargo/bin, after the project's own: its .venv or venv for the Python formatters, and its node_modules for Prettier and Biome. To use something else, give its command in the settings file: lsp.python = pylsp, or format.python = yapf --style=google. A command there always wins over these.

## Settings and transparency

Command comma opens ~/.config/minicode/settings.conf, written the first time with every setting listed and commented out. Changes apply as soon as the file is saved. Each panel has its own background color, opacity and text color; opacity applies only to the background, so text stays solid, and window.blur frosts what is behind the window. Syntax and Markdown colors can be changed too.

```
window.blur = true
editor.opacity = 70%
sidebar.opacity = 0.5
terminal.text = #E0E0E0
```

Every color in the file shows as a swatch, and clicking one opens the color picker, with an opacity slider for see-through colors; the window follows along as you drag. An opacity value is underlined with dots, and clicking it opens a slider. Either way the line is switched on for you if it was still commented out. A line with a mistake is ignored, and the status bar says which one and why.

The colors start from a theme, one of VS Code's built-in ones: Dark Modern (VS Code's own default), Dark+ (minicode's look, and the theme when none is set), Monokai, Solarized Dark, Tomorrow Night Blue, Light Modern, Light+, Quiet Light and Solarized Light. On the Mac, View > Theme switches between them, and so does typing the theme's name into the command palette; either way it is one line in the settings file, which you can also write yourself, here on Linux too:

```
theme = light-modern
```

Any color the file sets of its own wins over the theme's, so you can take a theme and change the few colors you would rather have. When you pick a theme and the file already sets colors, minicode asks whether to comment those lines out so the whole theme shows; they stay in the file to switch back on. A light theme gives the window light scroll bars, fields and menus too.

On the Mac you don't have to find the right line yourself. Right-click the file tree, the editor, the Markdown preview, the terminal, the browser, the status bar, the title bar, or the source control panel, and choose Adjust Settings at the bottom of the menu. The settings file opens with that part's keys in the middle of the pane and briefly highlighted, its first value selected. View > Adjust Settings lists the same parts.

![Picking new colors for the file tree, the editor and the status bar](docs/demos/settings.gif)

## Memory

The benchmark is a simple workload on this repository: clangd on a C++ file, some work in the terminal, typesetting a LaTeX document, and loading a Wikipedia page. The numbers are the RAM footprint on an M3 Mac, measured after a minute of settling:

|   | Memory |
| --- | --- |
| minicode | 320 MB |
| VS Code, Chrome and Preview | 1,283 MB |

Both sides run the same clangd. make membench reproduces it on scratch profiles. On disk, MiniCode.app is 1.7 MB and Visual Studio Code.app is 659 MB.

## Next to VS Code and Zed

minicode isn't VS Code or Zed, but I use it every day. Hotkeys alone move between writing code, running it in the terminal and reading GitHub in the browser pane. I edited my own resume in it, in LaTeX, and Claude Code runs fine in its terminal. It leaves out a good deal that VS Code and Zed offer, mostly to keep the footprint small: rename and find references, code actions, a debugger, tabs and split editors, extensions, and an AI assistant built into the editor. The git panel is small on purpose. It shows status, diffs and the commit graph with what is and isn't pushed, stages and unstages files, and commits, while checking out branches, merging and pushing stay in the terminal. For those, VS Code or Zed is the better choice.

Zed set out with a similar goal, a lighter and more usable IDE, and it does much of what minicode does. As of Zed 1.15 it has no built-in browser, which has been an open [feature request](https://github.com/zed-industries/zed/issues/10533) since 2024. LaTeX goes through an [extension](https://github.com/rzukic/zed-latex/wiki/Preview) that builds the PDF and shows it in a separate viewer such as Skim, with SyncTeX jumps between the two. VS Code works the same way, through extensions and outside viewers. minicode builds the browser, the PDF viewer and the LaTeX preview into the window instead, and text on the typeset page can be edited where it is.

| | minicode | Zed | VS Code |
| --- | --- | --- | --- |
| Language server | completion, diagnostics, hover, definition | full | full |
| Browser in the window | yes | no | a basic one |
| LaTeX | typeset in the window, edit on the page | extension, external PDF viewer | extension, PDF in a tab |
| Git | status, diffs, staging, committing and a read-only commit graph (macOS, Linux, and Android through Termux); branches, merges and pushing in the terminal | full | full |
| Debugger | no | yes | yes |
| Extensions | no | yes | yes |
| Platforms | macOS, Linux, Android | macOS, Linux, Windows | macOS, Linux, Windows |

## Installing and building

```
brew install --cask e-c-hansen/tap/minicode
```

The app is not notarized, since that needs a paid Apple Developer account, so the cask clears the quarantine flag and it opens without a Gatekeeper warning. The install also puts a minicode command on your PATH: give it folders or files, or nothing for the current directory, and `minicode -h` lists its options. In Finder, Open With offers minicode for text and source files, Markdown, LaTeX, images, PDFs, video, audio and folders, and you can drop any of them on its Dock icon. A file opens as a tab in the window whose folder already holds it, or else in the window in front, so `minicode notes.md` typed in minicode's own terminal opens it in that window; only with no window open does it get a new one. These are the rules VS Code's `code` command follows. As with `code`, `minicode -r` puts everything in the window in front, a folder included, and `minicode -n` opens a new window of its own. minicode never makes itself the default app for a kind of file; pick it under Open With in Finder's Get Info if you want a double click to use it.

To build it yourself you need only the Xcode command line tools.

```
git clone https://github.com/e-c-hansen/minicode.git
cd minicode
make
```

macOS remembers what an app may do, such as reading a folder in Documents or letting a program in its terminal use the microphone, by the app's signature. Run `scripts/make-signing-identity.sh` once before building and make signs with a certificate of your own in your login keychain, so those answers carry over from one build to the next; without it each build is signed ad hoc and asks again.

The demo folder has a Python file, C++, Markdown, a short LaTeX paper in demo/paper and a two file C++ example in demo/vec for trying clangd. make test runs the unit tests of the shared core. To run it:

```
make run DIR=demo
```

## Driving minicode from the command line, and agents

The same minicode command drives the copy that is running, which is how a coding agent such as Claude Code can show its work in minicode rather than describe it in a chat. Each command acts in the window in front (or the one named with `--window` or `--folder`), never takes the keyboard, and only ever adds: no tab or terminal you have is closed, replaced or moved.

```
minicode open src/main.mm:100              # a file at a line, by the usual open rules
minicode open src/Lsp.mm:3090 --to 3185 --note "Applies rename edits to every file"
minicode diff src/Lsp.mm                   # side by side against HEAD; --base REF, or two files
minicode term --run "make test"            # a new terminal, the command typed and run in plain sight
minicode term-output 3 --lines 50          # what a terminal shows, scrollback included
minicode browse http://localhost:8000      # the page in the browser pane beside the editor
minicode window --folder ~/project         # the window on a folder, opened if none shows it
minicode tour tour.json                    # a sequence of notes
minicode state | jq .windows[0].tabs       # windows, tabs, selection, terminals, browser
minicode selection                         # the selected text
```

A note is a line or two of plain text over a highlighted range, labelled with who wrote it (`--from`, Claude by default). It is never written into the file or its undo history, and it goes when you press Escape, click its ×, or edit inside its lines. Files opened with a note share one agent preview tab per window, its title in italics, which the next such open replaces, so an agent's output never piles up tabs; edit in it, or double-click its title, and it becomes an ordinary tab. A tour is a list of `{"path", "line", "to", "note"}` stops; Option+] and Option+[ step through it, and the note says where you are ("2 of 5"). `minicode help` describes every command, and they exit with 0 when done, 1 when the command failed, 2 for a usage error, and 3 when there is no minicode to talk to (one is started when none is running).

Each running minicode listens on a socket in your own temporary folder that only your user account can open, and nothing in minicode runs a command or a script by itself: `term --run` types what it was given into a terminal you can see. A script that sets up a workspace (a server in a terminal, its page in the browser, the README open) is just these commands in a file you run yourself.

The repository has a skill for Claude Code in skills/minicode that teaches it all of this: when reporting work, open the changed lines with a note, show the diff and run the tests in a terminal; when asked "where is X", open it rather than quote a path; read `state` and `selection` when you say "this". `make install` copies it to ~/.claude/skills/minicode.

## Linux

The Linux version, in the linux folder, is built on GTK4 with a VTE terminal, a WebKitGTK browser and poppler for PDFs. It has everything described above, including the LaTeX preview and language servers, with Control where the Mac uses Command. What it lacks is tabs, so a window holds one file at a time, typeset math in the Markdown preview, which shows the TeX for now, and blur behind the window, which GNOME gives applications no way to ask for.

Video and audio files play in the editor's pane through GTK's own GStreamer support. A stock Ubuntu plays WebM, Ogg, MP3 and FLAC out of the box; H.264 video and AAC audio, which most .mp4 and .m4a files hold, need `sudo apt install gstreamer1.0-libav`, and minicode says so when it meets one.

```
sudo apt install build-essential meson libgtk-4-dev \
    libvte-2.91-gtk4-dev libwebkitgtk-6.0-dev libpoppler-glib-dev pkg-config
cd linux
meson setup build
meson compile -C build
```

To run the demo:

```
./build/minicode ../demo
```

You can alias the local build, if you'd like:

```
alias minicode="$PWD/build/minicode"
```


[BUILD-LINUX.md](BUILD-LINUX.md) covers installing it as a desktop app and which shortcuts go to the terminal.

## Android

The Android port is made for phones with a physical keyboard, such as the Unihertz Titan 2 Elite. It's built around the same C++ core and offers approximately the same functionality: file tree, editor with syntax highlighting, images and PDF rendering, video and audio in Android's own player, a browser pane, and a terminal on a real shell, including all the special characters you need. The Markdown preview is the desktop's: links you can tap, real tables, pictures with GIFs playing, and a double tap on a paragraph, heading, list item, quote, code block or table cell opens a small box holding its Markdown to edit. Pictures with an https address are downloaded as on the desktop; since Android has no settings file, Web images in Markdown in the ⋮ menu turns that off. You can even tap links in the terminal for specific lines within files and go straight to them, like "main.cpp:42:7". The keyboard shortcuts work here too, so the browser, the editor, the file list and the terminal are each a key away. It feels like a tiny operating system.

With [Termux](https://f-droid.org/packages/com.termux/) installed, the terminal is Termux's own bash, in the folder you have open, so python, git, clang and anything else you installed with pkg run there, and vim, less and the Python prompt get a real terminal. Without Termux it is the phone's own shell, which has the basic file tools and nothing more. Termux also runs your language servers (clangd, pylsp and the rest) for squiggles, completion, hover and go to definition, typesets LaTeX with tectonic, including double tapping the page to edit the source behind it, and runs git for the same source control panel and commit graph the desktop has. The file list keeps up with the disk, so a file made in the terminal shows up in it straight away.

To install it, download MiniCode-*version*.apk from the [latest release](https://github.com/e-c-hansen/minicode/releases/latest) on your phone and open it; Android asks once whether your browser may install apps. [Obtainium](https://github.com/ImranR98/Obtainium) can install it from this repository's releases and keep it updated. It is not on the Play Store. [android/README.md](android/README.md) covers setting up Termux, the keyboard shortcuts, and building it yourself.

## Keyboard shortcuts

A blank cell means that port does not have the action yet. On Android, a letter alone means the leader key and then that letter, because a phone keyboard often has no Control: on a Unihertz Titan 2 the leader is the unlabelled key left of the right Shift, and elsewhere the Menu key. A held Ctrl works too, on a USB or Bluetooth keyboard or a phone whose Fn key is set to act as Ctrl. While the terminal has the keyboard, Ctrl and a letter go to the shell, so add Shift there. [android/README.md](android/README.md) has the rest.

| Action | macOS | Linux | Android |
| --- | --- | --- | --- |
| Add a cursor on the line below or above each cursor, at the same column | Option Command Down, Option Command Up |  |  |
| Add a cursor, or take one away, to type in several places at once | Option click, or Shift Command click |  |  |
| Add the next place the selected text occurs to the selection, or select the word at the cursor | Command D |  |  |
| Ask the inline agent: change the file at the cursor, write a terminal command, or hand a task to Claude Code | Command I |  |  |
| Browse the file tree and open the selected file | Up, Down, Return | Up, Down, Enter; Left and Right close and open folders |  |
| Clear the terminal, its scrollback included | Command K |  |  |
| Close an agent's note (`minicode open --note`) in the editor | Escape, or its × |  |  |
| Close the active terminal | Control Shift W |  |  |
| Close the application | Command Q | Ctrl Q |  |
| Close the tab, the terminal the keyboard is in, or the browser panel when it has the keyboard | Command W |  |  |
| Close the window | Shift Command W | Ctrl W |  |
| Comment or uncomment the selected lines | Command / | Ctrl / |  |
| Complete the word at the cursor, with a language server | Control Space, or Option Escape | Ctrl Space | N, or Ctrl N |
| Copy or cut the line the cursor is on when nothing is selected, and paste such a line above the cursor's line | Command C, Command X, then Command V |  |  |
| Copy the line, or the selected lines, below or above | Shift Command Down, Shift Command Up (or Shift Option Down, Up) |  |  |
| Delete the line, or every line the selection touches, at every cursor | Control Shift K |  |  |
| Delete to the start of the line, at every cursor | Command Delete |  |  |
| Delete the word before the cursor in the terminal's input line, or everything before it | Control W, Control U |  |  |
| Edit a column: a cursor on every line between two points, so typing adds the same text to each | Shift Command click at one end and Shift Command click (or drag) to the other, or Option drag; Escape goes back to one cursor |  |  |
| Export the typeset PDF of a LaTeX file | Shift Command S | Ctrl Shift S |  |
| Find across the whole folder | Shift Command F | Ctrl Shift F |  |
| Find every reference to the name at the cursor, with a language server | Shift F12 |  |  |
| Find in the current file, every match highlighted, Return and Shift Return for the next and previous one | Command F | Ctrl F |  |
| Find with match case, whole word, or a regular expression, in the find bar or Find in Folder | Option Command C, Option Command W, Option Command R |  |  |
| Find in the terminal, its scrollback included (Return and Shift Return for the next and previous match, Escape to close) | Command F, then Command G, Shift Command G |  |  |
| Find next, find previous | Command G, Shift Command G | Ctrl G or F3, Shift F3 |  |
| Focus the browser's address bar, opening the browser if it is hidden | Command 3 |  |  |
| Focus the editor, or the player when a video or audio file is open | Command 1 | Ctrl 1 |  |
| Focus the file tree | Command 0 | Ctrl 0 |  |
| Focus the terminal, opening it if it is hidden | Command 2 |  |  |
| Format the file, or the selected lines where the formatter can | Shift Option F |  |  |
| Go to a line of the open file, by its number (42:7 for a column too) | Control G |  |  |
| Go to definition | F12, or Command click (F12 may need fn on a laptop) | F12, or Ctrl click | G, or Ctrl G |
| Go to tab 1 to 8, or the last tab | Control 1 to 9, or Shift Command 1 to 9 (macOS keeps Shift Command 3, 4 and 5 for screenshots) |  |  |
| Go to the matching bracket, or to the closing bracket of the pair around the cursor | Shift Command backslash |  |  |
| Go to the next or previous problem in the file, with its message at the cursor | F8, Shift F8 (F8 may need fn on a laptop) |  |  |
| Go to the next or previous stop of an agent's tour (`minicode tour`) | Option ], Option [ |  |  |
| Go to the start or end of the file (Shift selects to it) | Command Home, Command End (fn Command Left, Right on a laptop) |  |  |
| Indent or outdent the selected lines | Tab, Shift Tab | Tab, Shift Tab | Tab, Shift Tab |
| Jump to the previous file |  | Ctrl Tab |  |
| Keep the agent preview tab as an ordinary tab | Double-click its title, or edit in it |  |  |
| List every problem the language servers report, grouped by file | Shift Command M, or click the problem counts in the status bar |  |  |
| List the TODO comments and open Markdown tasks in the folder | Shift Command D | Ctrl Shift L | W, or Ctrl Shift L |
| Move the line, or the selected lines, down or up, at every cursor | Option Down, Option Up, or Command Down, Command Up |  |  |
| Move the selected file to the Trash | Command Delete, in the file tree | Delete, in the file tree |  |
| Move to the next or previous terminal, across split panes and tabs | Option Command ], Option Command [ |  |  |
| Move to the start or end of the line at the terminal's prompt, or delete back to its start | Command Left, Command Right, Command Delete |  |  |
| New file | Command N | Ctrl Alt N | New file in the ⋮ menu |
| New folder | Shift Command N | Ctrl Shift N |  |
| New window | Option Command N | Ctrl N |  |
| Next tab, previous tab | Control Tab, Control Shift Tab, or Shift Command ], Shift Command [ |  |  |
| Open a file of the folder by typing part of its name, recently opened files first | Command P |  |  |
| Open a file or URL named in the terminal, such as src/main.cpp:42:7 | Command click it | Ctrl click it | Tap it |
| Open a folder | Command O | Ctrl O | O, or Ctrl O |
| Open a new terminal in a new tab | Control Shift backtick |  |  |
| Open the command palette, to find any menu command by name and run it | Shift Command P |  |  |
| Open the right click menu of what has the keyboard, at the selected row or the cursor | Shift F10, or Control Return, or the Menu key of a PC keyboard |  |  |
| Open the settings file | Command comma | Ctrl comma |  |
| Organize the file's imports, with a language server (Ruff's, when it runs) | Shift Option O |  |  |
| Page through the terminal's scrollback, or jump to its top or bottom | Shift Page Up, Shift Page Down, Command Home, Command End |  |  |
| Play or pause a video or audio file | Shift Command Space | Ctrl Shift Space | Space in the player, or Ctrl Shift Space |
| Refresh the file tree | Command R |  |  |
| Rename the name at the cursor wherever it is used, with a language server | F2 |  |  |
| Rename the selected file or folder | F2, in the file tree | F2, in the file tree |  |
| Reopen the tab closed last, where it was | Option Shift Command T |  |  |
| Replace across the whole folder, after a count of what will change | Option Shift Command F |  |  |
| Replace in the current file: the selected match and on to the next, or every match | Option Command F, then Return (or Shift Command 1) for one, Option Command Return for all |  |  |
| Request a change to minicode itself, made by Claude Code in a terminal | Shift Command K |  |  |
| Restart minicode, keeping its windows, tabs, unsaved changes and browser pages | The ↻ in the status bar, or Restart minicode in the minicode menu |  |  |
| Run the open file in the terminal | Shift Command Return |  |  |
| Save | Command S | Ctrl S | S, or Ctrl S |
| See the language servers, restart one, or choose the Python interpreter | Click the server in the status bar, or the minicode menu |  |  |
| Select a line, or every line dragged over, by its number | Click or drag in the line numbers, Shift click to extend |  |  |
| Select every place the selected text, or the word at the cursor, occurs | Shift Command L |  |  |
| Select the cursor's line, and the next line with each press after | Command L |  |  |
| Show or hide dotfiles | Shift Command . | Ctrl H, or Ctrl Shift . |  |
| Show or hide the on-screen keyboard, for symbols the phone's keyboard lacks |  |  | Y, or the keyboard button in the title bar |
| Show or hide the sizes of files and folders in the file tree | Shift Command I |  |  |
| Show or hide when each file in the file tree was created | Control Shift Command I |  |  |
| Show or hide when each file in the file tree was last saved, and when the open file was saved | Option Shift Command I |  |  |
| Show the language server's quick fixes for the cursor or the selection | Command period |  |  |
| Show the type and documentation under the cursor | Option Command I | Ctrl I | K, or Ctrl K |
| Split the terminal, with a new one beside it | Command backslash |  |  |
| Sort every keep-sorted block in the file | Shift F9 | Shift F9 | Sort keep-sorted blocks in the ⋮ menu |
| Sort the selected lines, or the keep-sorted block around the cursor | F9 (F9 may need fn on a laptop) | F9 | J, or Ctrl J |
| Take the selected completion | Tab; Return too after Control Space, after a dot, or once the arrow keys have moved through the list |  |  |
| Tick or clear a Markdown task, or make the selected lines tasks | Option Command L, or click its box in the preview | Ctrl L, or click its box in the preview | L, or Ctrl L, or tap its box in the preview |
| Toggle the browser | Shift Command B | Ctrl Shift B | B, or Ctrl Shift B |
| Toggle the editor, giving its space to the terminal and browser | Shift Command E | Ctrl Shift E |  |
| Toggle the file list or sidebar | Command B | Ctrl B | F, or Ctrl B or Ctrl F |
| Toggle the preview of a Markdown or LaTeX file | Shift Command V | Ctrl Shift P | P, or Ctrl P |
| Toggle the shortcut hints | Shift Command H | Ctrl Shift H | H, or Ctrl H |
| Toggle the source control panel in place of the file tree | Control Shift G | Ctrl Shift G | V, or Ctrl Shift G |
| Toggle the terminal | Shift Command T, Command J, or Control backtick | Ctrl Shift T, or Ctrl backtick | T, or Ctrl T |
| Undo, redo | Command Z, Shift Command Z | Ctrl Z, Ctrl Shift Z | U, R, or Ctrl Z, Ctrl Shift Z (Ctrl U, Ctrl R also work) |
| Use the selected text as what to find | Command E |  |  |
| Wrap the selected text in brackets or quotes, at every cursor | Type ( [ { " ' or a backtick with text selected |  |  |
| Zoom a PDF or the LaTeX preview in or out |  | Ctrl plus (or Ctrl equals), Ctrl minus, Ctrl with the scroll wheel, or a pinch on the touchpad |  |
| Zoom the editor's text in or out, or back to its size in the settings | Command equals (or Command plus), Command minus, Option Command 0 (or Command with keypad 0) |  |  |
| Zoom a PDF or the LaTeX preview back to the width of the pane |  | Ctrl Alt 0 |  |

## Limitations

minicode does what I use every day and leaves out a good deal. The terminal does not pass mouse clicks to full screen programs, and does not rewrap its lines when it gets narrower or wider. The Markdown preview follows GitHub's rules for emphasis, links, lists, quotes and tables, but only a handful of HTML tags mean anything to it, and on Linux and Android math shows as its TeX. In the LaTeX preview, text produced inside your own macro definitions cannot be edited from the page. Rename, find references, and quick fixes from a language server are on the Mac only so far.

## License

MIT.
