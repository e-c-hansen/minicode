project('minicode-gtk', 'cpp',
  version : '0.1.0',
  default_options : ['cpp_std=c++17', 'warning_level=2'])

# --- always-required dependency: GTK4 --------------------------------------
gtk_dep = dependency('gtk4')

# --- the portable C++ core, shared verbatim with the macOS build -----------
# These files live in ../src and are the single source of truth. We compile
# them directly; they are never copied into linux/.
core_dir = meson.current_source_dir() / '..' / 'src'
core_sources = [
  core_dir / 'SyntaxHighlighter.cpp',
  core_dir / 'MarkdownParser.cpp',
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
  sources : ['tests/run_tests.cpp', core_dir / 'SyntaxHighlighter.cpp'],
  include_directories : inc,
  install : false)
test('linux port core', port_tests)
