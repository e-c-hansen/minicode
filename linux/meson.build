project('mutible-gtk', 'cpp',
  version : '0.1.0',
  # debugoptimized, not Meson's default of debug: at -O0 the lexer is several
  # times slower, and typing /* over a 50,000-line file hitched for 0.1 s.
  default_options : ['cpp_std=c++17', 'warning_level=2', 'buildtype=debugoptimized'])

# --- always-required dependency: GTK4 --------------------------------------
gtk_dep = dependency('gtk4')

# --- the portable C++ core, shared verbatim with the macOS build -----------
# These files live in ../src and are the single source of truth. We compile
# them directly; they are never copied into linux/.
core_dir = meson.current_source_dir() / '..' / 'src'
core_sources = [
  core_dir / 'SyntaxHighlighter.cpp',
  core_dir / 'MarkdownParser.cpp',
  core_dir / 'MarkdownEdit.cpp',
  core_dir / 'MarkdownTasks.cpp',
  core_dir / 'MarkdownPage.cpp',
  core_dir / 'PreviewScroll.cpp',
  core_dir / 'MarkdownHtml.cpp',
  core_dir / 'MermaidDiagram.cpp',
  core_dir / 'MathTex.cpp',
  core_dir / 'Settings.cpp',
  core_dir / 'LineComments.cpp',
  core_dir / 'AutoIndent.cpp',
  core_dir / 'KeepSorted.cpp',
  core_dir / 'FolderSearch.cpp',
  core_dir / 'TextFind.cpp',
  core_dir / 'Regex.cpp',
  # The find bar's rules, several replacements over one text, and the word
  # at the caret that seeds the query (PARITY.md 1.6).
  core_dir / 'FindRules.cpp',
  core_dir / 'EditSpans.cpp',
  # A document's text and file rules (SESSION-DESIGN.md, stage 1): the
  # language server client's copy of each file is a Model::Text.
  core_dir / 'Text.cpp',
  core_dir / 'Document.cpp',
  # Each document's undo and redo (SESSION-DESIGN.md, stage 2), in place of
  # GtkTextBuffer's own (DocumentView.cpp records every buffer edit into it).
  core_dir / 'UndoHistory.cpp',
  # The session's host on its own thread, the editor's client of it, their
  # messages, and the transform that makes crossed edits agree
  # (SESSION-DESIGN.md, stage 4: the host owns each document's text and
  # undo, and the editor's buffer is its mirror).
  core_dir / 'Ot.cpp',
  core_dir / 'SessionProtocol.cpp',
  core_dir / 'SessionHost.cpp',
  core_dir / 'SessionClient.cpp',
  core_dir / 'SessionServer.cpp',
  # A browser's read-only view of a host (`mutible host --web`, stage 6b).
  core_dir / 'SessionWeb.cpp',
  core_dir / 'SessionWebPage.cpp',
  core_dir / 'SessionWebViewers.cpp',
  core_dir / 'SessionWebAccess.cpp',
  core_dir / 'SqliteRead.cpp',
  core_dir / 'SessionFiles.cpp',
  # The terminals' ptys belong to the host too (stage 5): a pty per shell,
  # and a headless screen with scrollback fed its bytes, whose marks give
  # the commands and whose replay rebuilds it for a renderer.
  core_dir / 'HostPty.cpp',
  core_dir / 'TerminalStream.cpp',
  core_dir / 'TerminalScreen.cpp',
  core_dir / 'MultiCaret.cpp',
  # A snippet completion's expansion and tab stops (SnippetSession.cpp and
  # LspSessionCompletion.cpp, PARITY.md 2.5).
  core_dir / 'Snippet.cpp',
  core_dir / 'LatexDoc.cpp',
  core_dir / 'LatexLog.cpp',
  core_dir / 'LatexScratch.cpp',
  core_dir / 'SyncTex.cpp',
  core_dir / 'Json.cpp',
  core_dir / 'Csv.cpp',
  core_dir / 'JsonOutline.cpp',
  core_dir / 'Notebook.cpp',
  core_dir / 'FileSniff.cpp',
  core_dir / 'HtmlPage.cpp',
  core_dir / 'LspClient.cpp',
  core_dir / 'LspProtocol.cpp',
  core_dir / 'LspText.cpp',
  core_dir / 'LspServers.cpp',
  core_dir / 'LspDiagnostics.cpp',
  core_dir / 'LspCompletion.cpp',
  core_dir / 'LspSync.cpp',
  core_dir / 'LspFeatures.cpp',
  core_dir / 'LspEdits.cpp',
  core_dir / 'TermLinks.cpp',
  # The control channel's table and parsing (ControlServer.cpp, the client).
  core_dir / 'ControlProtocol.cpp',
  # The Claude Code IDE link's protocol (IdeLinkServer.cpp).
  core_dir / 'IdeLink.cpp',
  core_dir / 'PageCheck.cpp',
  # Waiting for you: the hooks' JSON and the settings merge, and the board.
  core_dir / 'ClaudeHooks.cpp',
  core_dir / 'ClaudeUsage.cpp',
  core_dir / 'ClaudeStatusLine.cpp',
  core_dir / 'AttentionBoard.cpp',
  core_dir / 'AgentIndex.cpp',
  core_dir / 'AgentTranscript.cpp',
  core_dir / 'AgentEvidence.cpp',
  core_dir / 'AgentView.cpp',
  core_dir / 'InlineAgent.cpp',
  # Processes and ports: the table's rules and the /proc source (ProcessMonitor.cpp).
  core_dir / 'ProcessTable.cpp',
  core_dir / 'RunFile.cpp',
  core_dir / 'Runnables.cpp',
  core_dir / 'ProcFs.cpp',
  core_dir / 'GitStatus.cpp',
  core_dir / 'GitGraph.cpp',
  core_dir / 'GitModel.cpp',
  # The tree's listings: the splices a fresh listing makes, and their pace.
  core_dir / 'FileDetails.cpp',
  # Add Folder to Focus…: the folder chosen held to the window's folder
  # (FolderPicker::inScope).
  core_dir / 'FolderPicker.cpp',
  # The command registry and its Linux keys (Keys.cpp drives the menus and
  # the accelerators from them).
  core_dir / 'Commands.cpp',
  core_dir / 'CommandsLinux.cpp',
  # The tabs' rules (Tabs.cpp follows them, as the Mac's tabs do).
  core_dir / 'EditorTabs.cpp',
  # How the right side of a window is shared (WindowLayout.cpp shows it).
  core_dir / 'RightAreaLayout.cpp',
  core_dir / 'TerminalLayout.cpp',
  # A window's terminal dock in its record on the session's host.
  core_dir / 'TermDockRecord.cpp',
  # Getting around (PARITY.md 1.11): Go Back and Forward's history and what
  # a window records in it (NavHistoryUi.cpp drives it), and the zoom and
  # word wrap rules (Zoom.cpp keeps their state).
  core_dir / 'NavHistory.cpp',
  core_dir / 'NavRecorder.cpp',
  core_dir / 'ViewZoom.cpp',
  # Unsaved buffers kept on disk, and the tabs each folder had open
  # (BackupStore.cpp and Persistence.cpp keep them).
  core_dir / 'Backups.cpp',
  core_dir / 'TabSessions.cpp',
  core_dir / 'SavePlan.cpp',
  core_dir / 'BrowserTabs.cpp',
  core_dir / 'WebBookmark.cpp',
  # The gutter's line index, and the change bars it moves with the lines.
  core_dir / 'LineIndex.cpp',
  core_dir / 'PieceTable.cpp',
  core_dir / 'QuickDiff.cpp',
  core_dir / 'SideBySide.cpp',
  core_dir / 'LineDiff.cpp',
  # Merge conflict blocks and what keeping a side makes of them (the Mac's
  # editor and review page use them; the GTK port can follow).
  core_dir / 'MergeConflicts.cpp',
  # The review of uncommitted changes (ChangeReviewPage.cpp shows it).
  core_dir / 'ChangeReview.cpp',
  core_dir / 'ChangeReviewRead.cpp',
  # The palette's matching, modes and Quick Open's walk, and the outline
  # Go to Symbol lists without a server (PalettePanel.cpp shows them).
  core_dir / 'CommandPalette.cpp',
  core_dir / 'SymbolOutline.cpp',
  # Bracket boxes and Go to Bracket, bracket-pair colors, and auto-closing
  # pairs over the carets' rules (BracketView.cpp, BracketColors.cpp,
  # AutoCloseInput.cpp).
  core_dir / 'BracketMatch.cpp',
  core_dir / 'BracketPairs.cpp',
  core_dir / 'AutoClose.cpp',
  core_dir / 'MultiCaret.cpp',
  # A MultiEdit's ranges given to the highlighter one at a time (Document's
  # batches: typing at many carets, Replace All).
  core_dir / 'EditBatch.cpp',
  # Which words of a file are prose for a spell checker (the Mac's for now).
  core_dir / 'SpellCheck.cpp',
  # The regions a file folds into, the folds that are closed, and the limit
  # on what they hide (Folds.cpp shows them).
  core_dir / 'Folding.cpp',
  # Format Document and the folder's own tools (PARITY.md 2.3 and 2.4): the
  # plan, the decisions around it, the folder's question, the Python
  # environments, and what to install (Formatting.cpp, FormatRunner.cpp,
  # ConsentBar.cpp and LspSessionFolderTools.cpp use them).
  core_dir / 'Formatter.cpp',
  core_dir / 'FormatRun.cpp',
  core_dir / 'FolderTools.cpp',
  core_dir / 'PythonEnvs.cpp',
  core_dir / 'InstallHints.cpp',
]

# --- GTK GUI sources (always built) ----------------------------------------
# main.cpp is apart from the rest so that mutible-guitest can compile it again
# with main renamed, and share every other object with the real program.
main_source = 'src/main.cpp'
gui_sources = [
  'src/FileTree.cpp',
  # The tree following the active tab (explorer.auto-reveal).
  'src/TreeFollow.cpp',
  # A file row's icon from the system icon theme, by the core's FileIcon::Type.
  'src/TreeIcons.cpp',
  'src/ClickCount.cpp',
  # The editor: the view (Editor.cpp), each open file (DocumentView.cpp), the
  # tabs (Tabs.cpp) and their strip (TabStrip.cpp).
  'src/Editor.cpp',
  # The settings file's opacity values open a slider (PARITY.md 2.11).
  'src/OpacityPicker.cpp',
  'src/DocumentView.cpp',
  'src/Tabs.cpp',
  # The session's host and the editor's client of it (SESSION-DESIGN.md
  # stage 4a): the documents' text and undo live there.
  'src/SessionLink.cpp',
  'src/Groups.cpp',
  'src/TabStrip.cpp',
  # The view itself (CodeView.cpp: its drawing passes and the ruler), its
  # line-number gutter (Gutter.cpp) and each buffer's lines (BufferLines.cpp).
  'src/CodeView.cpp',
  'src/Gutter.cpp',
  'src/BufferLines.cpp',
  # Brackets: the boxes, Go to Bracket and the colors' painting
  # (BracketView.cpp), each document's colors (BracketColors.cpp), and
  # auto-closing pairs (AutoCloseInput.cpp), reading the buffer a unit a
  # character (BufferText.cpp).
  'src/BracketView.cpp',
  'src/BracketColors.cpp',
  'src/AutoCloseInput.cpp',
  'src/BufferText.cpp',
  # Several carets, the line commands and VS Code's words (PARITY.md 1.9).
  'src/Carets.cpp',
  'src/CaretEdits.cpp',
  # A snippet completion's tab stops in the view (PARITY.md 2.5).
  'src/SnippetSession.cpp',
  # Folding: each document's regions, folds and their invisible tag, and the
  # editor's placeholders, keys and commands (PARITY.md 1.10).
  'src/Folds.cpp',
  'src/EditorConflicts.cpp',
  'src/Markdown.cpp',
  'src/CsvTable.cpp',
  'src/HtmlView.cpp',
  'src/SqliteView.cpp',
  'src/JsonOutlineView.cpp',
  'src/MarkdownSide.cpp',
  # Terminal.cpp and Browser.cpp are always compiled but their bodies are
  # #ifdef-guarded, so without the feature macros they are empty and harmless.
  'src/Terminal.cpp',
  'src/TerminalFrame.cpp',
  'src/TerminalDock.cpp',
  'src/TerminalGroup.cpp',
  'src/Bookmarks.cpp',
  'src/Browser.cpp',
  'src/BrowserPages.cpp',
  'src/AppSettings.cpp',
  # Adjust Settings… in every right click menu, View > Theme, and what
  # they write (PARITY.md 2.11).
  'src/Appearance.cpp',
  # The keyboard back where it was after a choice from the menu bar.
  'src/MenuBarFocus.cpp',
  'src/ThemeCss.cpp',
  'src/Search.cpp',
  # The find and replace bar, and edits with several ranges (PARITY.md D5).
  'src/FindBar.cpp',
  'src/MultiEdit.cpp',
  # Language servers (Lsp.h lists the parts): the session's life (Lsp.cpp),
  # a server process (LspProcess.cpp), and the session split by concern as
  # the Mac's LspSession is (PARITY.md 2.0).
  'src/Lsp.cpp',
  'src/LspProcess.cpp',
  'src/LspSessionServers.cpp',
  'src/LspSessionSync.cpp',
  'src/LspSessionDiagnostics.cpp',
  'src/LspSessionCompletion.cpp',
  'src/LspSessionSignature.cpp',
  'src/LspSessionNavigation.cpp',
  # Rename, references and quick fixes, the workspace edit applier, and the
  # list and the box they show (PARITY.md 2.1).
  'src/LspSessionActions.cpp',
  'src/LspSessionEdits.cpp',
  'src/LspPicker.cpp',
  # The folder's own tools (PARITY.md 2.3): its .venv, and the code
  # rust-analyzer and the TypeScript server would run, only once the
  # folder's answer allows them (LspSessionFolderTools.cpp, the answer in
  # FolderConsent.cpp, the bar's question in ConsentBar.cpp).
  'src/LspSessionFolderTools.cpp',
  'src/FolderConsent.cpp',
  'src/ConsentBar.cpp',
  'src/FormatOffer.cpp',
  'src/ClaudeHooksOffer.cpp',
  # Questions whose text holds a path or a command (wrapped inside it).
  'src/Question.cpp',
  'src/InstallView.cpp',
  # Format Document (PARITY.md 2.4): the window's side (Formatting.cpp),
  # the formatter's process (FormatRunner.cpp), and a server's formatting
  # for files nothing else formats (LspSessionFormatting.cpp).
  'src/Formatting.cpp',
  'src/FormatRunner.cpp',
  'src/LspSessionFormatting.cpp',
  # Problems, the second server, watched files and Organize Imports
  # (PARITY.md 2.2): the list's rows and F8 (LspSessionProblems.cpp) and the
  # list itself (ProblemsPanel.cpp), Ruff beside the Python server and which
  # server answers what (LspSessionAlso.cpp), files changed on disk for the
  # servers' watchers (LspSessionWatch.cpp, the watcher's thread in
  # LspWatcher.cpp), and Organize Imports (LspSessionImports.cpp).
  'src/LspSessionProblems.cpp',
  'src/ProblemsPanel.cpp',
  'src/LspSessionAlso.cpp',
  'src/LspSessionWatch.cpp',
  'src/LspWatcher.cpp',
  'src/LspSessionImports.cpp',
  # Semantic tokens (PARITY.md 2.6): asked for by the session
  # (LspSessionSemantic.cpp), painted by each document (SemanticColors.cpp).
  'src/LspSessionSemantic.cpp',
  'src/SemanticColors.cpp',
  # Wrapped lines keep their indentation (#214, editor.wrapping-indent).
  'src/WrapIndentTags.cpp',
  # Symbols for the palette's "@" and "#" (LspSessionSymbols.cpp), and the
  # server's folding ranges (LspSessionFolding.cpp).
  'src/LspSessionSymbols.cpp',
  'src/LspSessionFolding.cpp',
  # JSON no server checks, checked by the editor itself (#213).
  'src/LspSessionJsonCheck.cpp',
  'src/MediaView.cpp',
  # PdfView.cpp is #ifdef-guarded like Terminal.cpp and Browser.cpp, and so
  # are the LaTeX preview's two files, which need poppler too.
  'src/PdfView.cpp',
  'src/PageWords.cpp',
  'src/LatexClick.cpp',
  'src/Latex.cpp',
  # Resolves the terminal's Ctrl+click file references; plain C++.
  'src/TermLinkPath.cpp',
  # The Source Control panel, over the core's GitModel (../src): the panel
  # (GitPanel.cpp), its commands (GitVerbs.cpp) and menus (GitMenus.cpp),
  # running git (GitRun.cpp), the Trash for Discard (GitTrash.cpp), and the
  # diff view with its hunk actions (GitDiffView.cpp).
  'src/GitPanel.cpp',
  'src/GitVerbs.cpp',
  'src/GitMenus.cpp',
  'src/GitRun.cpp',
  'src/EnvLock.cpp',
  'src/GitTrash.cpp',
  'src/GitDiffView.cpp',
  'src/CompareView.cpp',
  'src/ComparePanes.cpp',
  'src/CompareDecision.cpp',
  # The editor's change bars (GitGutter.cpp) and the tree's colors
  # (TreeGitColors.cpp), both watching the repository's folder
  # (GitDirWatch.cpp).
  'src/GitGutter.cpp',
  # The ▶ beside each test (TestGutter.cpp, over the core's Runnables).
  'src/TestGutter.cpp',
  'src/AgentNotes.cpp',
  'src/InlineAgentBar.cpp',
  'src/TerminalAttention.cpp',
  'src/TreeGitColors.cpp',
  'src/GitDirWatch.cpp',
  # Web pictures in the Markdown preview: the libsoup half (#ifdef-guarded,
  # like Terminal.cpp) over its plain C++ half.
  'src/WebImageRules.cpp',
  'src/WebImages.cpp',
  # Mermaid diagrams in the Markdown preview: mmdc, run and kept.
  'src/Mermaid.cpp',
  # Math in the Markdown preview: tectonic, run and cut with poppler.
  'src/Math.cpp',
  # The menu bar, the accelerators and the tree's keys, from the registry.
  'src/Keys.cpp',
  # The shortcut hints' search box.
  'src/HintsSearchBox.cpp',
  # The shortcut hints in columns, as many as fit the window.
  'src/HintsColumns.cpp',
  # The window's panes: the sidebar, and the editor, browser and terminal
  # laid out as on the Mac.
  'src/WindowLayout.cpp',
  # Between runs: each folder's tabs, the backups of unsaved tabs, and
  # auto-save (the Mac's BackupStore and EditorController+Persistence).
  'src/BackupStore.cpp',
  'src/Persistence.cpp',
  # Recent folders: File > Open Recent, and the dock icon's menu as desktop
  # actions in a copy of the desktop file (HANDOFF.md item 69).
  'src/RecentList.cpp',
  'src/DockMenu.cpp',
  # As the whole desktop (MUTIBLE_DESKTOP=1, linux/desktop/): Suspend,
  # Restart, Power Off and Log Out, and a window kept open (DesktopMode.h).
  'src/DesktopMode.cpp',
  # The first launch as mutible copies what minicode kept (src/Rename.h).
  'src/MigrationLinux.cpp',
  # The palette over the window: commands, Quick Open, Go to Line, symbols.
  'src/PalettePanel.cpp',
  # Big files and long lines: large-file mode, the question past 50 MB,
  # reading in the background, and a long line shown a part at a time.
  'src/LargeFileView.cpp',
  # The editor's and the terminals' zoom and the word wrap toggle, for
  # every window (Zoom.cpp), and Go Back and Forward (NavHistoryUi.cpp).
  'src/Zoom.cpp',
  # The control channel (PARITY.md 3.1): its socket, its handlers and the
  # client `mutible open|window|state|selection` runs (the Mac's CLI, shared).
  'src/ControlServer.cpp',
  'src/IdeLinkServer.cpp',
  'src/ControlCommands.cpp',
  'src/ProcessMonitor.cpp',
  'src/ProcessesPanel.cpp',
  'src/AgentModel.cpp',
  'src/AgentsPanel.cpp',
  # The panel's plan usage forecast and the status line log's two-press
  # install and undo (A13; the core's ClaudeUsage and ClaudeStatusLine).
  'src/UsageView.cpp',
  'src/StatusLineOffer.cpp',
  'src/EvidencePage.cpp',
  'src/AgentWorktrees.cpp',
  'src/ChangeReviewPage.cpp',
  '..' / 'tools' / 'mutible-cli.cpp',
  'src/NavHistoryUi.cpp',
  # The command line's options, and a terminal given back at once: the
  # plain C++ rules (LaunchOptions.cpp) and the fork (Detach.cpp).
  'src/LaunchOptions.cpp',
  'src/Detach.cpp',
  # MUTIBLE_STARTUP_LOG=1: where the time to a usable window goes (R3).
  # MUTIBLE_BENCH: the memory benchmark's workload (make membench-linux).
  'src/Bench.cpp',
  'src/StartupLog.cpp',
  # What a window puts off until it has drawn its first frame (R3).
  'src/WhenDrawn.cpp',
]

inc = include_directories('src', '../src')

# The session's host runs on a thread of its own (SessionLink.cpp).
deps = [gtk_dep, dependency('sqlite3'), dependency('threads')]
cpp_args = []

# --- optional: terminal panel via VTE (vte-2.91-gtk4) ----------------------
vte_dep = dependency('vte-2.91-gtk4', required : get_option('terminal'))
if vte_dep.found()
  deps += vte_dep
  cpp_args += '-DMUTIBLE_ENABLE_TERMINAL'
  message('Terminal panel: ENABLED (vte-2.91-gtk4 found)')
else
  message('Terminal panel: disabled (vte-2.91-gtk4 not found)')
endif

# --- optional: browser panel via WebKitGTK (webkitgtk-6.0) -----------------
webkit_dep = dependency('webkitgtk-6.0', required : get_option('browser'))
if webkit_dep.found()
  deps += webkit_dep
  cpp_args += '-DMUTIBLE_ENABLE_BROWSER'
  message('Browser panel: ENABLED (webkitgtk-6.0 found)')
else
  message('Browser panel: disabled (webkitgtk-6.0 not found)')
endif

# --- optional: PDF viewing via poppler-glib ----------------------------------
# Also what the LaTeX preview will draw its pages with. Without it a PDF gets
# the same "Cannot display" message as any other binary file.
poppler_dep = dependency('poppler-glib', required : get_option('pdf'))
if poppler_dep.found()
  deps += poppler_dep
  cpp_args += '-DMUTIBLE_ENABLE_PDF'
  message('PDF viewer: ENABLED (poppler-glib found)')
else
  message('PDF viewer: disabled (poppler-glib not found)')
endif

# --- optional: pictures from the web in the Markdown preview (libsoup 3) ----
# libsoup 3 is the HTTP library WebKitGTK 6 is built on (webkitgtk-6.0.pc
# requires libsoup-3.0), so wherever the browser panel builds, this is
# already installed and costs users no new package. It is its own option so
# a build without WebKit can still have it. Without it, web pictures show
# their alt text.
soup_dep = dependency('libsoup-3.0', required : get_option('webimages'))
if soup_dep.found()
  deps += soup_dep
  cpp_args += '-DMUTIBLE_ENABLE_WEB_IMAGES'
  message('Web pictures in the Markdown preview: ENABLED (libsoup-3.0 found)')
else
  message('Web pictures in the Markdown preview: disabled (libsoup-3.0 not found)')
endif

# The app's version, for the User-Agent those fetches send: the Mac's
# Info.plist, which scripts/release.sh stamps, is the one place it is kept.
fs = import('fs')
app_version = meson.project_version()
plist = fs.read('..' / 'Info.plist')
version_key = '<key>CFBundleShortVersionString</key>'
if plist.contains(version_key)
  app_version = plist.split(version_key)[1].split('<string>')[1].split('</string>')[0].strip()
endif
cpp_args += '-DMUTIBLE_VERSION="' + app_version + '"'

mutible_exe = executable('mutible',
  sources : [main_source] + gui_sources + core_sources,
  include_directories : inc,
  dependencies : deps,
  cpp_args : cpp_args,
  install : true)

# `mbl`, the short name, and `minicode`, the name before 1.6.0, which says
# once on a terminal that it is now mutible (src/Rename.h) and goes in a
# later release.
install_symlink('mbl', pointing_to : 'mutible', install_dir : get_option('bindir'))
install_symlink('minicode', pointing_to : 'mutible', install_dir : get_option('bindir'))   # rename-mutible: keep

# --- desktop integration ----------------------------------------------------
# Installs the launcher and its icons so the app shows up in the GNOME app grid
# and can be pinned to the dock. The .desktop file and the icon files are both
# named after the GTK application id (ai.ible.Mutible) because that is how
# Wayland compositors match a window back to its launcher; get the name wrong
# and the dock shows a second, generic icon for the running window.
app_id = 'ai.ible.Mutible'

desktop_conf = configuration_data()
desktop_conf.set('BINDIR', get_option('prefix') / get_option('bindir'))

configure_file(
  input         : 'resources' / (app_id + '.desktop.in'),
  output        : app_id + '.desktop',
  configuration : desktop_conf,
  install       : true,
  install_dir   : get_option('datadir') / 'applications')

# BROWSER in the app's terminals (PARITY.md 3.7): beside the binary in a build
# folder, and in share/mutible when installed.
configure_file(
  input  : 'resources' / 'mutible-browser.js',
  output : 'mutible-browser.js',
  copy   : true)
install_data('resources' / 'mutible-browser.js',
  install_dir  : get_option('datadir') / 'mutible',
  install_mode : 'rwxr-xr-x')

# The shell integration (PARITY.md 3.1, D10), found the same two ways and
# copied by Terminal.cpp into a folder of the process's own: bash's script
# (Linux only) and zsh's, which is the Mac's own file, never a copy that
# could drift.
configure_file(
  input  : 'resources' / 'mutible-bash-integration.bash',
  output : 'mutible-bash-integration.bash',
  copy   : true)
configure_file(
  input  : '..' / 'resources' / 'mutible-zsh-integration.zsh',
  output : 'mutible-zsh-integration.zsh',
  copy   : true)
install_data('resources' / 'mutible-bash-integration.bash',
             '..' / 'resources' / 'mutible-zsh-integration.zsh',
  install_dir : get_option('datadir') / 'mutible')

# The Claude Code skill, for `mutible install-skill`: beside the binary in a
# build folder, in share/mutible when installed.
configure_file(
  input  : '..' / 'skills' / 'mutible' / 'SKILL.md',
  output : 'SKILL.md',
  copy   : true)
install_data('..' / 'skills' / 'mutible' / 'SKILL.md',
  install_dir : get_option('datadir') / 'mutible' / 'skills' / 'mutible')

# PNGs extracted from the macOS resources/AppIcon.icns, which stays the single
# source of truth for the artwork.
foreach size : ['32x32', '64x64', '128x128', '256x256', '512x512']
  install_data(
    'resources' / 'icons' / size / (app_id + '.png'),
    install_dir : get_option('datadir') / 'icons' / 'hicolor' / size / 'apps')
endforeach

# --- tests: the pure-C++ pieces, no GTK and no display required -------------
# Covers the byte-offset -> character-offset conversion that maps
# SyntaxHighlighter output onto GtkTextBuffer iterators.
port_tests = executable('mutible-linux-tests',
  sources : ['tests/run_tests.cpp', 'src/ThemeCss.cpp',
             core_dir / 'SyntaxHighlighter.cpp', core_dir / 'Settings.cpp',
             core_dir / 'LineComments.cpp', 'src/PageWords.cpp',
             core_dir / 'LatexDoc.cpp', core_dir / 'SyncTex.cpp',
             core_dir / 'TermLinks.cpp',
  core_dir / 'GitStatus.cpp',
  core_dir / 'GitGraph.cpp', 'src/TermLinkPath.cpp', core_dir / 'GitModel.cpp',
  'src/WebImageRules.cpp', 'src/LaunchOptions.cpp', 'src/EnvLock.cpp',
  'src/MigrationLinux.cpp',
  # ThemeCss's popups read the find bar's palette (FindRules).
  core_dir / 'FindRules.cpp', core_dir / 'EditSpans.cpp', core_dir / 'MultiCaret.cpp',
  core_dir / 'TextFind.cpp', core_dir / 'Regex.cpp'],
  include_directories : inc,
  dependencies : [dependency('gio-2.0'), dependency('threads')],
  install : false)
test('linux port core', port_tests)

# The LaTeX preview's click sweep: double-clicks every word of a typeset PDF
# through the preview's own click path. Needs a typeset document, so it is
# not a test; tests/latex/sweep-linux.sh typesets one and runs it.
if poppler_dep.found()
  executable('mutible-latex-sweep',
    sources : ['tests/latex_sweep.cpp', 'src/PageWords.cpp', 'src/LatexClick.cpp',
               core_dir / 'LatexDoc.cpp', core_dir / 'SyncTex.cpp'],
    include_directories : inc,
    dependencies : [poppler_dep, dependency('gio-2.0')],
    cpp_args : ['-DMUTIBLE_ENABLE_PDF'],
    install : false)
endif

# --- the GUI checks (tests/gui/Harness.h): the whole app, offscreen ----------
# The objects are the real program's; only main.cpp is compiled again, with
# main renamed and the seam the checks reach the app through. Nothing here
# changes the mutible target. libxtst-dev is needed only to build them.
xtst_dep = dependency('xtst', required : false)
x11_dep = dependency('x11', required : false)
gtk_x11_dep = dependency('gtk4-x11', required : false)
if xtst_dep.found() and x11_dep.found() and gtk_x11_dep.found()
  gui_main = static_library('mutible-guitest-main',
    sources : [main_source],
    include_directories : inc,
    dependencies : deps,
    # The browser pane's home page must not reach the network from a check.
    cpp_args : cpp_args + ['-Dmain=mutible_main', '-DMUTIBLE_GUITEST',
                           '-DMUTIBLE_BROWSER_HOME="about:blank"'],
    install : false)

  # One line a suite, in tests/gui/suites.txt (fs.read makes Meson reconfigure
  # when that file changes, so a merged suite is built with no other step).
  gui_suites = []
  foreach line : fs.read('tests' / 'gui' / 'suites.txt').split('\n')
    name = line.strip()
    if name != '' and not name.startswith('#')
      gui_suites += 'tests' / 'gui' / name
    endif
  endforeach

  guitest = executable('mutible-guitest',
    sources : ['tests/gui/Harness.cpp', 'tests/gui/Input.cpp', 'tests/gui/Wrapper.cpp'] + gui_suites,
    objects : mutible_exe.extract_objects(gui_sources + core_sources),
    link_with : gui_main,
    include_directories : [inc, include_directories('tests/gui')],
    dependencies : deps + [xtst_dep, x11_dep, gtk_x11_dep],
    cpp_args : cpp_args,
    # backtrace() needs the names of the app's own functions.
    link_args : ['-rdynamic'],
    install : false)

  # The Mac's scripted language server (tests/gui/FakeLsp.h), plain C++ over
  # the core's JSON and framing, found beside mutible-guitest.
  fakelsp = executable('fakelsp',
    sources : ['..' / 'tests' / 'fakelsp' / 'fakelsp.cpp', core_dir / 'Json.cpp',
               core_dir / 'Text.cpp', core_dir / 'EditSpans.cpp',
               core_dir / 'LspClient.cpp', core_dir / 'LspProtocol.cpp',
               core_dir / 'LspText.cpp', core_dir / 'LspServers.cpp',
               core_dir / 'LspDiagnostics.cpp', core_dir / 'LspCompletion.cpp',
               core_dir / 'LspSync.cpp', core_dir / 'LspFeatures.cpp',
               core_dir / 'LspEdits.cpp'],
    include_directories : inc,
    install : false)

  # `meson test -C build` leaves these out (the setup below), so it stays
  # quick and needs no display; `meson test -C build --suite gui` runs them.
  # The harness has its own limits, well inside this one.
  # The detach suite runs the real mutible as well.
  test('gui', guitest, suite : 'gui', is_parallel : false, timeout : 1800,
       depends : [fakelsp, mutible_exe])
  add_test_setup('default', exclude_suites : ['gui'], is_default : true)
else
  message('GUI checks: not built (they need libxtst-dev)')
endif
