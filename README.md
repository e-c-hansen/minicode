# MiniCode

MiniCode is a small code editor for macOS, written from scratch in C++ and Objective-C++, with no Electron and no third-party dependencies. It links against Cocoa and WebKit, both of which ship with the operating system, and nothing else. The compiled binary is about 150 KB.

I built it because I wanted a lightweight place to browse a folder, read and edit files with syntax highlighting, preview Markdown, and have a terminal and a browser one keystroke away, without pulling in a few hundred megabytes of runtime to do it.

## What it does

You open a folder and get a file tree on the left, much like the explorer in VS Code. Click a file and it opens in the main pane. The tree is live, so anything you create, rename, or delete elsewhere, in the built in terminal, in git, or in another program, shows up on its own within a moment, because it watches the folder with FSEvents rather than taking a one time snapshot. You can also manage files from the tree directly, right click for new file, new folder, rename, move to trash, and reveal in Finder, or use the same items from the File menu. Source files are syntax highlighted based on their extension, so Python, C, C++, Objective-C, JavaScript, TypeScript, JSON, shell scripts, and a handful of others get colored keywords, strings, comments, numbers, and so on. Markdown files render as formatted text right in the window, and you can flip between the rendered view and the raw source when you want to edit them.

Files are editable, not just viewable. Type into a file and the highlighting updates as you go, save with Command S, and undo and redo work as you would expect. The title bar shows a dot when you have unsaved changes.

There is a terminal you can pull up at the bottom with Control backtick, and you can drag the bar above it to resize. It runs a single persistent zsh session, so it behaves like a normal shell across commands. Change directory and you stay there, export a variable or define a function and it is still around for the next command. It keeps your command history on the up and down arrows, and clicking anywhere in the panel drops the cursor on the input line. It is still meant as a quick command runner rather than a full terminal emulator, so it does not host interactive full screen programs like vim or htop, but for the everyday things you reach for, git, make, ls, running a script, it works the way you would want. There is also an embedded browser you can toggle with Shift Command B, which is a real WebKit view with a URL bar and back, forward, and reload buttons.

If you ever forget a shortcut, press Control H and a small panel lists the ones available in your current context. The panel is aware of what you are doing, so the Markdown preview toggle only shows up when a Markdown file is open, for instance.

## Installing and building

You need the Xcode command line tools, which give you clang. Nothing else is required. Clone the repo and run make.

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
| Command W | Close window |
| Shift Command W | Close application |
| Command 0 | Focus the file tree |
| Command 1 | Focus the editor |
| Up, Down, Return | Browse the tree and open the selected file |
| Control Tab | Jump to the previous file |
| Control Command N | New file |
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

## Honest limitations

This is an MVP, and it is scoped like one. The terminal keeps a persistent shell so state carries across commands, but it runs each command with its input closed, which means interactive full screen programs like vim or htop are out of scope, and it strips color and other escape codes rather than rendering them. The Markdown parser covers the common cases rather than the whole CommonMark spec. Syntax highlighting is based on file extension and covers a fixed set of languages. None of these are hard to extend, they are just where the line got drawn for a first version.

## License

MIT.
