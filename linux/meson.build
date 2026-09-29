project('minicode-gtk', 'cpp',
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
  core_dir / 'Settings.cpp',
  core_dir / 'LineComments.cpp',
  core_dir / 'FolderSearch.cpp',
  core_dir / 'LatexDoc.cpp',
  core_dir / 'SyncTex.cpp',
  core_dir / 'Json.cpp',
  core_dir / 'LspClient.cpp',
  core_dir / 'TermLinks.cpp',
  core_dir / 'GitStatus.cpp',
  core_dir / 'GitGraph.cpp',
]

# --- GTK GUI sources (always built) ----------------------------------------
gui_sources = [
  'src/main.cpp',
  'src/FileTree.cpp',
  'src/Editor.cpp',
  'src/Markdown.cpp',
  # Terminal.cpp and Browser.cpp are always compiled but their bodies are
  # #ifdef-guarded, so without the feature macros they are empty and harmless.
  'src/Terminal.cpp',
  'src/Browser.cpp',
  'src/AppSettings.cpp',
  'src/ThemeCss.cpp',
  'src/Search.cpp',
  'src/Lsp.cpp',
  'src/MediaView.cpp',
  # PdfView.cpp is #ifdef-guarded like Terminal.cpp and Browser.cpp, and so
  # are the LaTeX preview's two files, which need poppler too.
  'src/PdfView.cpp',
  'src/PageWords.cpp',
  'src/LatexClick.cpp',
  'src/Latex.cpp',
  # Resolves the terminal's Ctrl+click file references; plain C++.
  'src/TermLinkPath.cpp',
  # The Source Control panel (GitPanel) over its plain C++ half (GitModel).
  'src/GitModel.cpp',
  'src/GitPanel.cpp',
  # Web pictures in the Markdown preview: the libsoup half (#ifdef-guarded,
  # like Terminal.cpp) over its plain C++ half.
  'src/WebImageRules.cpp',
  'src/WebImages.cpp',
]

inc = include_directories('src', '../src')

deps = [gtk_dep]
cpp_args = []

# --- optional: terminal panel via VTE (vte-2.91-gtk4) ----------------------
vte_dep = dependency('vte-2.91-gtk4', required : get_option('terminal'))
if vte_dep.found()
  deps += vte_dep
  cpp_args += '-DMINICODE_ENABLE_TERMINAL'
  message('Terminal panel: ENABLED (vte-2.91-gtk4 found)')
else
  message('Terminal panel: disabled (vte-2.91-gtk4 not found)')
endif

# --- optional: browser panel via WebKitGTK (webkitgtk-6.0) -----------------
webkit_dep = dependency('webkitgtk-6.0', required : get_option('browser'))
if webkit_dep.found()
  deps += webkit_dep
  cpp_args += '-DMINICODE_ENABLE_BROWSER'
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
  cpp_args += '-DMINICODE_ENABLE_PDF'
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
  cpp_args += '-DMINICODE_ENABLE_WEB_IMAGES'
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
cpp_args += '-DMINICODE_VERSION="' + app_version + '"'

executable('minicode',
  sources : gui_sources + core_sources,
  include_directories : inc,
  dependencies : deps,
  cpp_args : cpp_args,
  install : true)

# --- desktop integration ----------------------------------------------------
# Installs the launcher and its icons so the app shows up in the GNOME app grid
# and can be pinned to the dock. The .desktop file and the icon files are both
# named after the GTK application id (org.minicode.Editor) because that is how
# Wayland compositors match a window back to its launcher; get the name wrong
# and the dock shows a second, generic icon for the running window.
app_id = 'org.minicode.Editor'

desktop_conf = configuration_data()
desktop_conf.set('BINDIR', get_option('prefix') / get_option('bindir'))

configure_file(
  input         : 'resources' / (app_id + '.desktop.in'),
  output        : app_id + '.desktop',
  configuration : desktop_conf,
  install       : true,
  install_dir   : get_option('datadir') / 'applications')

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
port_tests = executable('minicode-linux-tests',
  sources : ['tests/run_tests.cpp', 'src/ThemeCss.cpp',
             core_dir / 'SyntaxHighlighter.cpp', core_dir / 'Settings.cpp',
             core_dir / 'LineComments.cpp', 'src/PageWords.cpp',
             core_dir / 'LatexDoc.cpp', core_dir / 'SyncTex.cpp',
             core_dir / 'TermLinks.cpp',
  core_dir / 'GitStatus.cpp',
  core_dir / 'GitGraph.cpp', 'src/TermLinkPath.cpp', 'src/GitModel.cpp',
  'src/WebImageRules.cpp'],
  include_directories : inc,
  install : false)
test('linux port core', port_tests)

# The LaTeX preview's click sweep: double-clicks every word of a typeset PDF
# through the preview's own click path. Needs a typeset document, so it is
# not a test; tests/latex/sweep-linux.sh typesets one and runs it.
if poppler_dep.found()
  executable('minicode-latex-sweep',
    sources : ['tests/latex_sweep.cpp', 'src/PageWords.cpp', 'src/LatexClick.cpp',
               core_dir / 'LatexDoc.cpp', core_dir / 'SyncTex.cpp'],
    include_directories : inc,
    dependencies : [poppler_dep, dependency('gio-2.0')],
    cpp_args : ['-DMINICODE_ENABLE_PDF'],
    install : false)
endif
