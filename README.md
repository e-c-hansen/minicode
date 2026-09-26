# MiniCode

MiniCode is a quick, 1 MB workspace application with a text editor, terminal, browser, and file browser navigable by keyboard. It's for writing/running code, browsing/editing documents, making/editing LaTeX, and browsing the web. It runs on macOS, Linux, and Android. It's written from scratch in C++ with no Electron and no third-party dependencies. The macOS app links only against frameworks that ship with the system. The Linux version is a GTK4 port. The Android version is a Kotlin app. They all share the same C++ core. The rest of this README describes the macOS app; the [Linux](#linux) and [Android](#android) sections say how the others differ.

I built it because I wanted one light place to browse a folder, edit code with a language server behind it, preview Markdown and LaTeX, and keep a terminal and a browser a keystroke away, without a few hundred megabytes of runtime underneath.

![Clicking through the demo folder in MiniCode](docs/demos/tour.gif)

## What it does

Open a folder and you get a file tree on the left and the file you click in the main pane. The tree follows changes made elsewhere, in the terminal, in git or in another program, and right clicking an item offers new file, new folder, rename, move to trash, reveal in Finder and copy path.

Source files are highlighted by extension: Python, the C family, JavaScript, TypeScript, JSON, shell, LaTeX and a few others. Markdown renders as formatted text, with working links, tables and images, and Shift Command P flips to the source. You can also edit the rendered page: double click a paragraph, heading, list item or table cell, and a small editor opens holding its Markdown. Images and PDFs open in the same pane, and reload when the file changes on disk.

![Opening a PNG and then a PDF from the file tree](docs/demos/files.gif)

Control backtick opens a terminal at the bottom. It runs one persistent zsh with your own startup files, so aliases, history and state carry over from one command to the next, and colors come through. When a program wants the whole screen (vim, less, htop, git log's pager, ssh), the panel becomes a character grid until it exits.

![The terminal panel running ls, git log and a Python script](docs/demos/terminal.gif)

![Writing a short list in vim inside the terminal panel, then paging through git log](docs/demos/vim.gif)

Paths and URLs in the output are links. Command click src/main.cpp:42:7 from a compiler, or a file Claude Code mentions, and it opens at that line; a URL opens in the browser panel, which Shift Command B toggles.

![Command clicking a grep result to open it at its line, then a URL that opens in the browser panel](docs/demos/links.gif)

Control Shift G swaps the file tree for a small source control panel, on the Mac and on Linux. It shows the branch, how far it is ahead of or behind its upstream, and the changed files in two lists, staged and not yet staged, each with a one letter status. Arrow keys move through the files, Return shows the selected file's diff in the main pane, Space stages or unstages it, and Command Return (Ctrl Enter on Linux) in the message box commits. Git does the work underneath, and when it refuses something, such as a commit with nothing staged, its own message appears in the panel.

Under the changes is the commit graph: the history of the current branch and its upstream, drawn in colored lanes the way VS Code's graph and `git log --graph` draw them, with branch, remote and tag labels on their commits. Commits you have not pushed yet are tinted blue with a hollow dot and an up arrow, commits on the upstream you have not pulled are tinted green with a down arrow, and a line above the graph says how many of each there are, or that the branch has no upstream. A checkbox shows every local and remote branch instead. The graph loads 200 commits at a time, with a row at the bottom for the next 200. Tab moves between the changes, the graph and the message box, the Down arrow goes from the last changed file into the graph, and Return or a click on a commit shows its author, date, message and diff in the main pane. The panel only reads history; checking out, branching, merging, pulling and pushing are left to the terminal.

Shift Command H lists the shortcuts that apply right now.

## LaTeX

Open a .tex file and MiniCode typesets it and shows the PDF where the editor sits, again after every change. Shift Command P flips between the page and the source.

You can also edit from the page. Double click some text (a heading, a sentence, a table cell, a list entry, an equation) and a small editor opens holding the LaTeX behind it. Change it, press Return, and only those bytes of your document are replaced, so macros and packages MiniCode does not understand are left alone. Inside a list it also offers to add an entry. If MiniCode cannot tell which piece of source a word came from, it opens nothing rather than the wrong text. Nothing is written to disk until you press Command S, and Shift Command S exports the PDF.

![Editing a list entry by double clicking it in the typeset page](docs/demos/latex.gif)

Typesetting is done by [tectonic](https://tectonic-typesetting.github.io/). If it is not installed, the preview offers to download it (about 22 MB), or you can run brew install tectonic. The first document downloads the packages it needs, and after that it works offline.

## Language servers

With a language server installed, MiniCode gives you completion, errors as you type, hover and go to definition. It knows clangd (C, C++, Objective-C), pyright or pylsp (Python), gopls (Go), rust-analyzer (Rust) and typescript-language-server (JavaScript and TypeScript), and finds them on your PATH and in the usual Homebrew, Cargo and Go folders.

Problems get a red or yellow squiggle, and the status bar counts them. Typing a dot, an arrow or a double colon opens completion, and Control Space opens it anywhere. F12 or Command click jumps to a definition, and Command I shows the type and documentation under the cursor.

![clangd flagging an error, completing a member, showing hover info and jumping to a definition](docs/demos/lsp.gif)

The settings file can pick a different server or turn one off (lsp.python = pylsp, lsp.go = off, lsp.enabled = false).

## Settings and transparency

Command comma opens ~/.config/minicode/settings.conf, written the first time with every setting listed and commented out. Changes apply as soon as the file is saved. Each panel has its own background color, opacity and text color; opacity applies only to the background, so text stays solid, and window.blur frosts what is behind the window. Syntax and Markdown colors can be changed too.

```
window.blur = true
editor.opacity = 70%
sidebar.opacity = 0.5
terminal.text = #E0E0E0
```

Every color in the file shows as a swatch, and clicking one opens the color picker; the window follows along as you drag. A line with a mistake is ignored, and the status bar says which one and why.

![Picking new colors for the file tree, the editor and the status bar](docs/demos/settings.gif)

## Memory

I benchmarked a simple workload using this very repository: clangd on a C++ file, interacting with the terminal, typesetting a LaTeX document, and loading a Wikipedia page. Here's the RAM footprint on an M3 Mac after giving the machine a minute to reach a steady state:

|   | Memory |
| --- | --- |
| MiniCode | 320 MB |
| VS Code, Chrome and Preview | 1,283 MB |

Both sides run the same clangd. make membench reproduces it on scratch profiles. On disk, MiniCode.app is 1.3 MB and Visual Studio Code.app is 659 MB.

## Next to VS Code and Zed

MiniCode isn't VS Code or Zed, but I do use it every day. I can jump between writing code, running it in terminal, and browsing github repos via web browser via hotkeys, alone. I edited my own resume in LaTeX. Claude Code runs fine in MiniCode's terminal, too. With that said, MiniCode leaves out some things that VS Code and Zed offer, usually for sake of keeping a tight footprint and avoiding becoming the bloatware that drove me to make it in the first place. For instance, there's no means to rename and find references, formatting and code actions, a debugger, tabs and split editors, extensions, and an AI assistant built into the editor. The git panel is deliberately small: it shows status, diffs and the commit graph with what is and is not pushed, stages and unstages files, and commits, while checking out branches, merging and pushing stay in the terminal. VS Code or Zed are better options for those particulars.

Zed was created with a similar goal for a more usable, lightweight IDE. It accomplishes much of what MiniCode seeks out to do, but, as of Zed 1.15, there is no built in browser, which has been an open [feature request](https://github.com/zed-industries/zed/issues/10533) since 2024. LaTeX goes through an [extension](https://github.com/rzukic/zed-latex/wiki/Preview) that builds the PDF and shows it in a separate viewer such as Skim, with SyncTeX jumps between the two. It begins to feel like the clunky, emulated workarounds that VS Code extensions offered during my time as a VS Code user. That's why MiniCode's browser is built-in, yet lightweight. It's why PDFs render easily. Why LaTeX is in-line mutable.

| | MiniCode | Zed | VS Code |
| --- | --- | --- | --- |
| Language server | completion, diagnostics, hover, definition | full | full |
| Browser in the window | yes | no | a basic one |
| LaTeX | typeset in the window, edit on the page | extension, external PDF viewer | extension, PDF in a tab |
| Git | status, diffs, staging, committing and a read-only commit graph (macOS and Linux); branches, merges and pushing in the terminal | full | full |
| Debugger | no | yes | yes |
| Extensions | no | yes | yes |
| Platforms | macOS, Linux, Android | macOS, Linux, Windows | macOS, Linux, Windows |

## Installing and building

```
brew install --cask e-c-hansen/tap/minicode
```

The app is not notarized, since that needs a paid Apple Developer account, so the cask clears the quarantine flag and it opens without a Gatekeeper warning. The install also puts a minicode command on your PATH: give it a folder or a file, or nothing for the current directory.

To build it yourself you need only the Xcode command line tools.

```
git clone https://github.com/e-c-hansen/minicode.git
cd minicode
make
```

The demo folder has a Python file, C++, Markdown, a short LaTeX paper in demo/paper and a two file C++ example in demo/vec for trying clangd. make test runs the unit tests of the shared core. To run it:

```
make run DIR=demo
```

## Linux

The Linux version, in the linux folder, is built on GTK4 with a VTE terminal, a WebKitGTK browser and poppler for PDFs. It has everything described above, including the LaTeX preview and language servers, with Control where the Mac uses Command. The one thing it lacks is blur behind the window, which GNOME gives applications no way to ask for.

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

As a foray into the mobile space, I made an Android port, specifically for keyboard-based phones like the Unihertz Titan 2 Elite. It's built around the same C++ core and offers approximately the same functionality: file tree, editor with syntax highlighting, Markdown preview, images and PDF rendering, a browser pane, and a terminal on a real shell, including all the special characters you need. You can even tap links in the terminal for specific lines within files and go straight to them, like "main.cpp:42:7". The keyboard shortcuts work here, too. You can quickly jump between browser, text editor, file system, and so on. It feels like a tiny operating system.

With [Termux](https://f-droid.org/packages/com.termux/) installed, it also runs your language servers (clangd, pylsp and the rest) for squiggles, completion, hover and go to definition, and typesets LaTeX with tectonic, including double tapping the page to edit the source behind it.

To install it, download MiniCode-*version*.apk from the [latest release](https://github.com/e-c-hansen/minicode/releases/latest) on your phone and open it; Android asks once whether your browser may install apps. [Obtainium](https://github.com/ImranR98/Obtainium) can install it from this repository's releases and keep it updated. It is not on the Play Store. [android/README.md](android/README.md) covers setting up Termux, the keyboard shortcuts, and building it yourself.

## Keyboard shortcuts

A blank cell means that port does not have the action yet. On Android, shortcuts start with a leader key, because a phone keyboard often has no Control: on a Unihertz Titan 2 it is the unlabelled key left of the right Shift, and elsewhere the Menu key. [android/README.md](android/README.md) has the rest.

| Action | macOS | Linux | Android, after the leader key |
| --- | --- | --- | --- |
| Browse the file tree and open the selected file | Up, Down, Return | Up, Down, Enter; Left and Right close and open folders |  |
| Close the application | Shift Command W | Ctrl Q |  |
| Close the window | Command W | Ctrl W |  |
| Comment or uncomment the selected lines | Command / | Ctrl / |  |
| Complete the word at the cursor, with a language server | Control Space, or Option Escape | Ctrl Space | N |
| Export the typeset PDF of a LaTeX file | Shift Command S | Ctrl Shift S |  |
| Find across the whole folder | Shift Command F | Ctrl Shift F |  |
| Find in the current file | Command F | Ctrl F |  |
| Find next, find previous | Command G, Shift Command G | Ctrl G or F3, Shift F3 |  |
| Focus the editor | Command 1 | Ctrl 1 |  |
| Focus the file tree | Command 0 | Ctrl 0 |  |
| Go to definition | F12, or Command click (F12 may need fn on a laptop) | F12, or Ctrl click | G |
| Jump to the previous file | Control Tab | Ctrl Tab |  |
| Move the selected file to the Trash | Command Delete | Delete, in the file tree |  |
| New file | Control Command N | Ctrl Alt N |  |
| New folder | Shift Command N | Ctrl Shift N |  |
| New window | Command N | Ctrl N |  |
| Open a file or URL named in the terminal, such as src/main.cpp:42:7 | Command click it | Ctrl click it | Tap it |
| Open a folder | Command O | Ctrl O | O |
| Open the settings file | Command comma | Ctrl comma |  |
| Refresh the file tree | Command R |  |  |
| Rename the selected file or folder |  | F2, in the file tree |  |
| Save | Command S | Ctrl S | S |
| Show or hide dotfiles | Shift Command . | Ctrl H, or Ctrl Shift . |  |
| Show the type and documentation under the cursor | Command I | Ctrl I | K |
| Toggle the browser | Shift Command B | Ctrl Shift B | B |
| Toggle the editor, giving its space to the terminal and browser | Shift Command E | Ctrl Shift E |  |
| Toggle the file list or sidebar | Command B | Ctrl B | F |
| Toggle the preview of a Markdown or LaTeX file | Shift Command P | Ctrl Shift P | P |
| Toggle the shortcut hints | Shift Command H | Ctrl Shift H | H |
| Toggle the source control panel in place of the file tree | Control Shift G | Ctrl Shift G |  |
| Toggle the terminal | Shift Command T, or Control backtick | Ctrl Shift T, or Ctrl backtick | T |
| Undo, redo | Command Z, Shift Command Z | Ctrl Z, Ctrl Shift Z |  |
| Zoom a PDF or the LaTeX preview in or out |  | Ctrl plus (or Ctrl equals), Ctrl minus, Ctrl with the scroll wheel, or a pinch on the touchpad |  |
| Zoom a PDF or the LaTeX preview back to the width of the pane |  | Ctrl Alt 0 |  |

## Limitations

MiniCode does what I use every day and leaves out a good deal. The terminal does not pass mouse clicks to full screen programs, and has no scrollback while a program has the grid. The Markdown parser covers the common cases, not all of CommonMark. In the LaTeX preview, text produced inside your own macro definitions cannot be edited from the page. The language server client has no rename, find references, formatting or code actions yet.

## License

MIT.
