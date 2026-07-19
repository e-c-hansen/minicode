# MiniCode

MiniCode is a small code editor for macOS, written from scratch in C++ and Objective-C++, with no Electron and no third-party dependencies. It links against Cocoa and WebKit, both of which ship with the operating system, and nothing else. The compiled binary is about 150 KB.

I built it because I wanted a lightweight place to browse a folder, read and edit files with syntax highlighting, preview Markdown, and have a terminal and a browser one keystroke away, without pulling in a few hundred megabytes of runtime to do it.

## What it does

You open a folder and get a file tree on the left, much like the explorer in VS Code. Click a file and it opens in the main pane. The tree is live, so anything you create, rename, or delete elsewhere, in the built in terminal, in git, or in another program, shows up on its own within a moment, because it watches the folder with FSEvents rather than taking a one time snapshot. You can also manage files from the tree directly, right click for new file, new folder, rename, move to trash, and reveal in Finder, or use the same items from the File menu. Source files are syntax highlighted based on their extension, so Python, C, C++, Objective-C, JavaScript, TypeScript, JSON, shell scripts, and a handful of others get colored keywords, strings, comments, numbers, and so on. Markdown files render as formatted text right in the window, and you can flip between the rendered view and the raw source when you want to edit them.

Files are editable, not just viewable. Type into a file and the highlighting updates as you go, save with Command S, and undo and redo work as you would expect. The title bar shows a dot when you have unsaved changes.

There is a terminal you can pull up at the bottom with Control backtick, and you can drag the bar above it to resize. It runs a single persistent zsh session, so it behaves like a normal shell across commands. Change directory and you stay there, export a variable or define a function and it is still around for the next command. It keeps your command history on the up and down arrows, and clicking anywhere in the panel drops the cursor on the input line. It is still meant as a quick command runner rather than a full terminal emulator, so it does not host interactive full screen programs like vim or htop, but for the everyday things you reach for, git, make, ls, running a script, it works the way you would want. There is also an embedded browser you can toggle with Shift Command B, which is a real WebKit view with a URL bar and back, forward, and reload buttons.

If you ever forget a shortcut, press Control H and a small panel lists the ones available in your current context. The panel is aware of what you are doing, so the Markdown preview toggle only shows up when a Markdown file is open, for instance.

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

There is also a make run target that opens the current directory, and you can pass a folder with make run DIR=~/some/project.

## A quick tour

The repo includes a demo folder with a Python file, a C++ file, and a Markdown file, so you can see the highlighting and the Markdown rendering right away. Launch the app against it and click through the three files.

```
make run DIR=demo
```

Click hello.py and sample.cpp to see the syntax coloring, then click README.md to see it rendered. Press Shift Command P to switch that Markdown file to its raw source, make an edit, save with Command S, and press Shift Command P again to see the change. Pull up the terminal with Control backtick and run something like ls or git status. Toggle the browser with Shift Command B and type a domain into the URL bar.

## Keyboard shortcuts

| Shortcut | Action |
| --- | --- |
| Command N | New window |
| Command O | Open folder |
| Command S | Save |
| Command Z, Shift Command Z | Undo, redo |
| Command F | Find in the current file |
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
| Command B | Collapse or restore the sidebar |
| Command T, or Control backtick | Toggle terminal |
| Shift Command B | Toggle browser |
| Shift Command P | Toggle Markdown preview |
| Shift Command H | Toggle the shortcut hints |

## How it is put together

The parts that do not need a graphical interface are plain C++. The syntax tokenizer lives in SyntaxHighlighter.h and .cpp, and it is a small hand written lexer that picks a grammar from the file extension and walks the text once, emitting colored ranges. The Markdown parser lives in MarkdownParser.h and .cpp, and it handles the common subset of Markdown, headings, bold and italic, inline and fenced code, lists, blockquotes, rules, and links.

The graphical layer is Objective-C++, which is the normal way to drive AppKit from C++. EditorController.mm owns the window, the file tree, and the editor, and it translates the ranges from the C++ core into colored text. Terminal.mm is the command runner, built on NSTask. Browser.mm wraps a WKWebView. The file tree, the panels, and the resizable terminal dock are laid out by hand rather than through nested split views, which turned out to be more predictable.

Because the highlighter and the Markdown parser are plain C++ with no dependencies, they can be tested on their own, away from the GUI. Run make test to build and run the suite in tests/run_tests.cpp, which checks the tokenizer against a few languages and the parser against headings, inline styles, code blocks, lists, tables, and block separation. The harness is a handful of macros, no test framework, and it exits non zero if anything fails.

## Distributing it

There are two ways to get MiniCode onto someone else's Mac, and which you need depends on who they are.

The simplest is to hand them the source and let them build it. A locally built app is not quarantined by Gatekeeper, so git clone, make, open MiniCode.app just works with no signing involved. This fits the from-scratch spirit of the project and costs nothing, and it is the recommended path for sharing with a few people who are comfortable running make.

You can also hand out a prebuilt app for free, without any Apple Developer account. make builds an ad-hoc signed app (free, and required to run at all on Apple Silicon), and make dist-zip or make dmg packages it for a GitHub Release. The one catch is that macOS quarantines anything downloaded from the internet, so the first time a recipient opens an unsigned build they need to either right-click the app and choose Open, or run this once:

    xattr -dr com.apple.quarantine MiniCode.app

That is the normal path for free and open-source Mac apps, and it is all that is needed.

Homebrew makes even that step disappear. It is published as a tap, so anyone can install with brew install --cask e-c-hansen/tap/minicode. Homebrew quarantines downloads by default, but because it is a private tap the cask clears the quarantine flag itself in a postflight step, so the app opens cleanly with no right-click and no notarization. The cask lives in the tap repo; a copy is kept at packaging/minicode.rb for reference.

The only thing free distribution cannot give you is a clean, warning-free double-click for people who download it, because that specifically requires notarization, which requires the paid Apple Developer Program membership. If you want that, the whole flow is scripted: scripts/sign-and-notarize.sh signs the app with your Developer ID and the hardened runtime, submits the disk image to Apple for notarization, and staples the result so it opens cleanly on any Mac. Read the comments at the top of that script for the one-time certificate and credential setup.

So the honest summary is that distribution is a solved, scripted problem here. Free build-from-source or a downloadable zip with the one-line unquarantine step covers almost everyone, and the only thing behind the paywall is the zero-friction double-click.

## Honest limitations

This is an MVP, and it is scoped like one. The terminal keeps a persistent shell so state carries across commands, but it runs each command with its input closed, which means interactive full screen programs like vim or htop are out of scope, and it strips color and other escape codes rather than rendering them. The Markdown parser covers the common cases rather than the whole CommonMark spec. Syntax highlighting is based on file extension and covers a fixed set of languages. None of these are hard to extend, they are just where the line got drawn for a first version.

## License

MIT.
