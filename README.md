# MiniCode

MiniCode is a small code editor for macOS, written from scratch in C++ and Objective-C++, with no Electron and no third-party dependencies. It links only against frameworks that ship with the operating system (Cocoa, WebKit, PDFKit, CoreServices) and the system zlib. The compiled binary is about 1 MB.

I built it because I wanted a lightweight place to browse a folder, read and edit files with syntax highlighting, preview Markdown and LaTeX, and have a terminal and a browser one keystroke away, without pulling in a few hundred megabytes of runtime to do it.

![Clicking through the demo folder in MiniCode](docs/demos/tour.gif)

Clicking through the demo folder: the README rendered, then shown as source with Shift Command P, a line added to hello.py and saved, and then sample.cpp.

## What it does

You open a folder and get a file tree on the left, much like the explorer in VS Code. Click a file and it opens in the main pane. The tree is live, so anything you create, rename, or delete elsewhere, in the built in terminal, in git, or in another program, shows up on its own within a moment, because it watches the folder with FSEvents rather than taking a one time snapshot. You can also manage files from the tree directly, right click for new file, new folder, rename, move to trash, and reveal in Finder, or use the same items from the File menu. Source files are syntax highlighted based on their extension, so Python, C, C++, Objective-C, JavaScript, TypeScript, JSON, shell scripts, and a handful of others get colored keywords, strings, comments, numbers, and so on. Markdown files render as formatted text right in the window, and you can flip between the rendered view and the raw source when you want to edit them. LaTeX files are typeset to a PDF in the same pane, and you can edit the document by double clicking the text on the page.

Files are editable, not just viewable. Type into a file and the highlighting updates as you go, save with Command S, and undo and redo work as you would expect. The title bar shows a dot when you have unsaved changes.

There is a terminal you can pull up at the bottom with Control backtick, and you can drag the bar above it to resize. It runs a single persistent zsh session on a real pseudo terminal, with your own startup files loaded, so aliases and functions from your .zshrc are there and state carries across commands. Output streams in as it is printed, Control C interrupts whatever is running, and programs that ask for input get it, including password prompts, which switch the input line to hidden text. A command that fails shows its exit status underneath. It keeps your command history on the up and down arrows, and clicking anywhere in the panel drops the cursor on the input line. Colors come through, so git, ls, test runners, and anything else that styles its output look the way they do in any other terminal, and progress lines that redraw themselves update in place. Ordinary output is kept as a scrolling log you can select and copy from. When a program wants the whole screen, vim, less, man, htop, git log with its pager, a Python prompt, or an ssh session, the panel switches to a real character grid for as long as that program needs it, and your keys go straight to it: arrows, Tab, Escape, the function keys, Control letters, and Option as Meta. Command shortcuts still reach the menus, so Command C copies a selection you drag in the grid and Command V pastes. When the program exits, the log comes back exactly as you left it. There is also an embedded browser you can toggle with Shift Command B, which is a real WebKit view with a URL bar and back, forward, and reload buttons.

![The terminal panel running ls, git log and a Python script](docs/demos/terminal.gif)

The terminal under a Python file, running ls, git log and the script itself, with the colors each command prints.

If you ever forget a shortcut, press Shift Command H and a small panel lists the ones available in your current context. The panel is aware of what you are doing, so the preview toggle only shows up when a Markdown or LaTeX file is open, for instance.

## LaTeX

Open a .tex file and MiniCode typesets it and shows you the PDF, right where the editor sits. Shift Command P flips between the typeset page and the source, the same way it does for Markdown. Every time the document changes it is typeset again, which takes well under a second for a short paper.

The interesting part is that the preview is not read only. Double click a piece of text on the page, a title, a heading, a sentence, a table cell, a list entry, or an equation, and a small editor opens holding the LaTeX that produced it. Change it, press Return, and that text is replaced in your document and the page is typeset again. When you click inside a list, the editor also offers to add an entry, which writes a new item after the one you clicked, indented to match the ones around it. Text inside italics, bold, links and your own macros can be clicked the same way, and so can words with accents or ligatures, or ones TeX hyphenated across two lines. If MiniCode cannot tell which piece of source a word came from, it opens nothing rather than the wrong text.

![Editing a list entry by double clicking it in the typeset page](docs/demos/latex.gif)

Double clicking a list entry in the PDF, rewriting it, and pressing Return. The page is typeset again, and Shift Command P shows the edit sitting in the source.

What you type in that editor is LaTeX, not plain text, so you can write \\emph{like this}, and a stray percent sign or ampersand needs its backslash the way it would anywhere else. An edit only ever replaces the bytes of the piece you clicked; the document is never regenerated from a model of it, so macros, packages and anything else MiniCode does not understand cannot be disturbed by an edit somewhere else in the file. Edits mark the buffer as changed, like typing does, and nothing is written to disk until you press Command S.

Typesetting is done by [tectonic](https://tectonic-typesetting.github.io/), a single binary TeX engine, in the same spirit as the language server support leaning on servers you already have. If you have it installed, MiniCode uses it. If you do not, the preview says so and offers to download the official build, about 22 MB, into ~/Library/Application Support/MiniCode. You can also install it yourself with brew install tectonic. The first document you typeset downloads the LaTeX packages it needs, roughly 40 MB for an ordinary paper, which tectonic keeps for later, so after that it works offline.

While it is previewing, MiniCode typesets a hidden copy of the buffer next to your file, named like .paper.minicode.tex, so that \\input and \\includegraphics still find their files and your own file is never written behind your back. The copy is deleted as soon as tectonic finishes.

## Language servers

MiniCode speaks the Language Server Protocol, so if you have a language server installed it gives you completion, go to definition, and error underlines as you type. MiniCode only implements the client; the servers are separate programs, the same way tectonic is for LaTeX. It knows these out of the box: clangd for C, C++ and Objective-C, which comes with the Xcode command line tools, pyright or pylsp for Python, gopls for Go, rust-analyzer for Rust, and typescript-language-server for JavaScript and TypeScript. It looks for them on your PATH and in the usual Homebrew, Cargo and Go folders, since an app started from the Dock gets a much shorter PATH than your shell.

Open a file and its server starts in the background, rooted at the folder the window has open. Problems show as a squiggly underline, red for errors and yellow for warnings, and resting the pointer on one shows the message. The status bar says which server is running and how many errors and warnings the file has. Typing a dot, an arrow, or a double colon opens a completion list, and Control Space opens it anywhere. Use the arrows to move through it, Return or Tab to take an item, and Escape to close it. F12, or Command click on a name, jumps to its definition, opening another file if that is where it lives. Command I shows the type and documentation of whatever is under the cursor.

If a language has no server installed, nothing changes except a short note in the status bar. You can choose a different server, or turn one off, in the settings file, for example lsp.python = pylsp, or lsp.go = off, and lsp.enabled = false turns the whole thing off. Servers are shut down when their window closes and when MiniCode quits.

## Settings and transparency

Press Command comma to open the settings file, which lives at ~/.config/minicode/settings.conf. The first time, MiniCode writes it out with every setting listed, commented out, and set to its default, so you can see what is available and uncomment what you want. Changes apply as soon as the file is saved, whether you save it in MiniCode or in any other editor, and every open window picks them up.

Each part of the window, the title bar, the file tree, the editor, the terminal, the browser toolbar, and the status bar, has its own background color, opacity, and text color. Opacity applies only to the background, so text stays solid while whatever is behind the window shows through. Setting window.opacity changes every panel at once, and a panel's own opacity overrides it. Turning on window.blur frosts what is behind the window, the way the macOS sidebar and Terminal do, which keeps text readable over a busy desktop. The syntax highlighting colors and the Markdown heading, link, code, and quote colors can be changed too.

```
window.blur = true
editor.opacity = 70%
sidebar.opacity = 0.5
terminal.opacity = 0.6
terminal.text = #E0E0E0
titlebar.opacity = 0.4
```

You don't need to think in hex codes. In the settings file every color is shown as a small swatch of itself, and clicking one opens the macOS color picker. As you pick, the line is rewritten, switched on if it was commented out, and saved, so the window changes while you drag around the color wheel. The opacity slider in the picker writes the alpha channel too.

![Picking new colors for the file tree, the editor and the status bar](docs/demos/settings.gif)

Picking new colors for the file tree, the editor and the status bar from their swatches. Each line is saved as it changes, and the window follows along.

If a line has a mistake in it, that line is ignored, and the status bar says which line and why. The rest of the file still applies. The system menu bar at the very top of the screen belongs to macOS, so apps cannot change it, but the window's own title bar is fully configurable.

## Installing and building

If you just want to install it, there is a Homebrew tap:

```
brew install --cask e-c-hansen/tap/minicode
```

The app is ad-hoc signed but not notarized, and the cask clears the macOS quarantine flag on install, so it opens without a Gatekeeper warning.

To build it yourself you need the Xcode command line tools, which give you clang. Nothing else is required. Clone the repo and run make.

```
git clone https://github.com/e-c-hansen/minicode.git
cd minicode
make
```

That produces MiniCode.app in the project directory. You can double click it in Finder, or run it from the command line and point it at a folder.

```
./MiniCode.app/Contents/MacOS/MiniCode ~/some/project
```

The Homebrew install also puts a minicode command on your PATH. Give it a folder to open that folder, or a file to open that file with its folder listed in the sidebar. With no argument it opens the current directory.

```
minicode ~/some/project
minicode ~/some/project/notes.md
```

There is also a make run target that opens the current directory, and you can pass a folder with make run DIR=~/some/project.

## A quick tour

The repo includes a demo folder with a Python file, a C++ file, a Markdown file, and a short LaTeX document in demo/paper, so you can see the highlighting and the previews right away. Launch the app against it and click through the three files.

```
make run DIR=demo
```

Click hello.py and sample.cpp to see the syntax coloring, then click README.md to see it rendered. Press Shift Command P to switch that Markdown file to its raw source, make an edit, save with Command S, and press Shift Command P again to see the change. Pull up the terminal with Control backtick and run something like ls or git status. Toggle the browser with Shift Command B and type a domain into the URL bar.

## Recording the demo GIFs

The GIFs in this README are recorded by the app itself, so they can be made again whenever the interface changes.

```
make demos
make demos SCENES="tour latex"
```

Each scene is a short script in src/Demo.mm that clicks files in the tree, types, and presses shortcuts the way a person would, through the same events a keyboard and mouse send. It only runs when the MINICODE_DEMO environment variable names a scene, which scripts/demos.sh does, so a normal launch never touches it. While a scene plays, the window is captured about ten times a second, and tools/makegif.m turns the frames into a GIF with a single palette, repeated frames merged, and only the changed part of each frame stored, which keeps each one well under a megabyte. The pointer and the key captions in the GIFs are drawn by the demo, since a window capture shows neither.

A window really does appear on screen for each scene, for about twenty seconds, and keyboard and mouse input from you is ignored while it plays. Everything the app reads is a scratch copy: a copy of the demo folder under /tmp, turned into a small git repository for the terminal scene, a separate home folder for the shell, the settings in tools/demo-settings.conf, and a copy of the tectonic cache. So nothing from your own home folder can show up in a frame. The LaTeX scene is skipped if tectonic is not installed. Recording needs the Screen Recording permission for the terminal you run make from. Set POSTERS=1 to also write a still PNG of each scene next to its GIF.

Adding a scene is a function of a few lines in src/Demo.mm and an entry in its scene table; make demos picks it up from there.

## Keyboard shortcuts

| Shortcut | Action |
| --- | --- |
| Command N | New window |
| Command O | Open folder |
| Command S | Save |
| Command Z, Shift Command Z | Undo, redo |
| Command F | Find in the current file |
| Shift Command F | Find across the whole folder |
| Command G, Shift Command G | Find next, find previous |
| Command W | Close window |
| Shift Command W | Close application |
| Command 0 | Focus the file tree |
| Command 1 | Focus the editor |
| Up, Down, Return | Browse the tree and open the selected file |
| Control Tab | Jump to the previous file |
| Control Command N | New file |
| Shift Command N | New folder |
| Command Delete | Move the selected file to the Trash |
| Command R | Refresh the file tree |
| Shift Command . | Show or hide dotfiles |
| Command B | Collapse or restore the sidebar |
| Shift Command E | Hide or restore the file editor; the terminal and browser take its space |
| Shift Command T, or Control backtick | Toggle terminal |
| Shift Command B | Toggle browser |
| Shift Command P | Toggle the preview, for Markdown and LaTeX files |
| Shift Command H | Toggle the shortcut hints |
| Command comma | Open the settings file |
| Command slash | Comment or uncomment the selected lines |
| Control Space, or Option Escape | Complete the word at the cursor, when the file has a language server |
| F12, or Command click | Go to definition (on a laptop keyboard F12 may need the fn key) |
| Command I | Show the type and documentation under the cursor |

## How it is put together

The parts that do not need a graphical interface are plain C++. LineComments.cpp works out which comment marker a file uses and toggles comments across a selection, keeping the block's indentation lined up. The syntax tokenizer lives in SyntaxHighlighter.h and .cpp, and it is a small hand written lexer that picks a grammar from the file extension and walks the text a line at a time, emitting colored ranges. Each line remembers whether it ended inside a block comment or an unfinished string, so when you type, only the lines you touched are lexed again, plus any below them whose colors really changed. A keystroke in a 100,000 line file costs a fraction of a millisecond, and opening a comment at the top of a large file recolors everything under it at once. The Markdown parser lives in MarkdownParser.h and .cpp, and it handles the common subset of Markdown, headings, bold and italic, inline and fenced code, lists, blockquotes, rules, and links. The LaTeX side is two more of these: LatexDoc.h and .cpp reads a document into the byte ranges that hold editable text, and performs the edits, while SyncTex.h and .cpp reads the file TeX writes beside the PDF so that a point on the page can be traced back to a line of source. Latex.mm is the part that needs a GUI, running tectonic and showing the result with PDFKit.

The graphical layer is Objective-C++, which is the normal way to drive AppKit from C++. EditorController.mm owns the window, the file tree, and the editor, and it translates the ranges from the C++ core into colored text. Terminal.mm runs zsh on a pseudo terminal, and TerminalStream.cpp, which is plain C++ with its own unit tests, reads the output and picks out the markers the shell sends to say where each command starts and ends. TerminalScreen.cpp, also plain C++, reads the same bytes as a grid of character cells for full screen programs, and TerminalGridView.mm draws that grid and turns key presses into what the program expects. Browser.mm wraps a WKWebView. The language server client is split the same way: Json.cpp and LspClient.cpp are plain C++ that frame messages, track the handshake, the open documents and the outstanding requests, and read the replies, all tested against a scripted server, while Lsp.mm runs the server processes and draws what they report. Settings.cpp, also plain C++ and unit tested, parses the settings file and works out the final color of each panel, and AppSettings.mm watches the file and tells the windows to redraw when it changes. The file tree, the panels, and the resizable terminal dock are laid out by hand rather than through nested split views, which turned out to be more predictable.

Because the highlighter and the Markdown parser are plain C++ with no dependencies, they can be tested on their own, away from the GUI. Run make test to build and run the suite in tests/run_tests.cpp, which checks the tokenizer against a few languages, replays thousands of random edits against real files to make sure incremental highlighting always matches a full pass, the LaTeX reader and the SyncTeX reader, the parser against headings, inline styles, code blocks, lists, tables, and block separation, the terminal output reader, the terminal screen, which is fed byte streams recorded from real vim and less sessions, the JSON reader and the language server client, and the settings parser, including a check that every setting documented in the default file is one the parser accepts. The harness is a handful of macros, no test framework, and it exits non zero if anything fails.

## Distributing it

There are two ways to get MiniCode onto someone else's Mac, and which you need depends on who they are.

The simplest is to hand them the source and let them build it. A locally built app is not quarantined by Gatekeeper, so git clone, make, open MiniCode.app just works with no signing involved. This fits the from-scratch spirit of the project and costs nothing, and it is the recommended path for sharing with a few people who are comfortable running make.

You can also hand out a prebuilt app for free, without any Apple Developer account. make builds an ad-hoc signed app (free, and required to run at all on Apple Silicon), and make dist-zip or make dmg packages it for a GitHub Release. The one catch is that macOS quarantines anything downloaded from the internet, so the first time a recipient opens an unsigned build they need to either right-click the app and choose Open, or run this once:

    xattr -dr com.apple.quarantine MiniCode.app

That is the normal path for free and open-source Mac apps, and it is all that is needed.

Homebrew makes even that step disappear. It is published as a tap, so anyone can install with brew install --cask e-c-hansen/tap/minicode. Homebrew quarantines downloads by default, so the cask clears the quarantine flag itself after installing, and the app opens cleanly with no right-click and no notarization. The build itself is attached to this repository's releases, next to the tag it came from, and the tap holds only the cask, which is written from packaging/minicode.rb when a release is cut.

The only thing free distribution cannot give you is a clean, warning-free double-click for people who download it, because that specifically requires notarization, which requires the paid Apple Developer Program membership. If you want that, the whole flow is scripted: scripts/sign-and-notarize.sh signs the app with your Developer ID and the hardened runtime, submits the disk image to Apple for notarization, and staples the result so it opens cleanly on any Mac. Read the comments at the top of that script for the one-time certificate and credential setup.

So the honest summary is that distribution is a solved, scripted problem here. Free build-from-source or a downloadable zip with the one-line unquarantine step covers almost everyone, and the only thing behind the paywall is the zero-friction double-click.

## Honest limitations

This is an MVP, and it is scoped like one. The terminal handles full screen programs, but it does not yet pass mouse clicks to them, and while a program has the grid there is no scrollback of its own; the scroll wheel sends arrow keys instead, which is what vim and less want. The Markdown parser covers the common cases rather than the whole CommonMark spec. The LaTeX preview understands enough of the language to find the text you clicked, which covers ordinary documents well, but text produced by your own macros is shown without being editable, and undo in the preview is its own stack, so Command Z steps back through preview edits while the page is up, and through your typing when the source view is. Syntax highlighting is based on file extension and covers a fixed set of languages. The language server support sends the whole file on every change rather than just the edit, which is fine for ordinary files, and it has no rename, references, formatting or code actions yet. None of these are hard to extend, they are just where the line got drawn for a first version.

## License

MIT.
