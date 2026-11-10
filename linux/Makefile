# Plain Makefile fallback for people who don't want Meson.
#
#   make                     # core viewer: tree + editor + markdown (GTK4 only)
#   make TERMINAL=1          # add the VTE terminal panel
#   make BROWSER=1           # add the WebKitGTK browser panel
#   make PDF=1               # add the PDF viewer (poppler-glib)
#   make WEBIMAGES=1         # pictures from https:// in the Markdown preview (libsoup-3.0)
#   make TERMINAL=1 BROWSER=1 PDF=1 WEBIMAGES=1
#   make run DIR=~/some/dir
#   make clean
#
# Meson (../linux/meson.build) is the primary, better-tested build. This exists
# so a minimal `apt install libgtk-4-dev` + `make` gets you the core viewer.

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2
# Deliberately not `build`: that is Meson's directory, and `make clean` would
# delete it out from under a Meson tree in the same checkout.
BUILD    := build-make
BIN      := $(BUILD)/minicode

# Shared portable core (never copied — compiled straight from ../src). Keep it
# the same list as core_sources in meson.build: CI builds only with Meson, so
# a file missing here is noticed by nobody until someone runs this.
CORE := ../src/SyntaxHighlighter.cpp ../src/MarkdownParser.cpp ../src/MarkdownEdit.cpp \
        ../src/MarkdownTasks.cpp ../src/Settings.cpp ../src/LineComments.cpp \
        ../src/AutoIndent.cpp ../src/KeepSorted.cpp ../src/FolderSearch.cpp ../src/TextFind.cpp \
        ../src/Regex.cpp ../src/FindRules.cpp ../src/EditSpans.cpp \
        ../src/LatexDoc.cpp ../src/SyncTex.cpp \
        ../src/Json.cpp ../src/LspClient.cpp ../src/TermLinks.cpp \
        ../src/LspProtocol.cpp ../src/LspText.cpp ../src/LspServers.cpp \
        ../src/LspDiagnostics.cpp ../src/LspCompletion.cpp ../src/LspSync.cpp \
        ../src/LspFeatures.cpp ../src/LspEdits.cpp \
        ../src/GitStatus.cpp ../src/GitGraph.cpp ../src/GitModel.cpp \
        ../src/FileDetails.cpp ../src/Commands.cpp ../src/CommandsLinux.cpp \
        ../src/EditorTabs.cpp ../src/Backups.cpp ../src/TabSessions.cpp \
        ../src/LineIndex.cpp ../src/QuickDiff.cpp ../src/LineDiff.cpp \
        ../src/RightAreaLayout.cpp \
        ../src/NavHistory.cpp ../src/NavRecorder.cpp ../src/ViewZoom.cpp \
        ../src/CommandPalette.cpp ../src/SymbolOutline.cpp \
        ../src/BracketMatch.cpp ../src/BracketPairs.cpp ../src/AutoClose.cpp \
        ../src/MultiCaret.cpp ../src/EditBatch.cpp ../src/Folding.cpp \
        ../src/Formatter.cpp ../src/FormatRun.cpp ../src/FolderTools.cpp \
        ../src/PythonEnvs.cpp ../src/InstallHints.cpp

# GUI sources. Terminal.cpp / Browser.cpp / PdfView.cpp and the LaTeX preview's
# LatexClick.cpp / Latex.cpp are always listed but compile to nothing unless
# their feature macro is defined.
GUI := src/main.cpp src/FileTree.cpp src/Editor.cpp src/Document.cpp src/Tabs.cpp \
       src/TabStrip.cpp src/CodeView.cpp src/Gutter.cpp src/BufferLines.cpp src/Markdown.cpp \
       src/BracketView.cpp src/BracketColors.cpp src/AutoCloseInput.cpp src/BufferText.cpp \
       src/Carets.cpp src/CaretEdits.cpp src/Folds.cpp \
       src/Terminal.cpp src/Browser.cpp src/AppSettings.cpp src/ThemeCss.cpp \
       src/Search.cpp src/FindBar.cpp src/MultiEdit.cpp src/MediaView.cpp src/PdfView.cpp src/PageWords.cpp \
       src/LatexClick.cpp src/Latex.cpp src/TermLinkPath.cpp \
       src/Lsp.cpp src/LspProcess.cpp src/LspSessionServers.cpp src/LspSessionSync.cpp \
       src/LspSessionDiagnostics.cpp src/LspSessionCompletion.cpp src/LspSessionNavigation.cpp \
       src/LspSessionActions.cpp src/LspSessionEdits.cpp src/LspPicker.cpp \
       src/LspSessionFolderTools.cpp src/FolderConsent.cpp src/ConsentBar.cpp \
       src/Formatting.cpp src/FormatRunner.cpp src/LspSessionFormatting.cpp \
       src/GitPanel.cpp src/WebImageRules.cpp src/WebImages.cpp src/Keys.cpp \
       src/BackupStore.cpp src/Persistence.cpp src/WindowLayout.cpp \
       src/PalettePanel.cpp \
       src/LargeFileView.cpp src/LaunchOptions.cpp src/Detach.cpp \
       src/StartupLog.cpp src/WhenDrawn.cpp \
       src/Zoom.cpp src/NavHistoryUi.cpp

SRCS := $(GUI) $(CORE)

PKGS   := gtk4
# The version the User-Agent of web picture fetches carries, from the Mac's
# Info.plist, which the release script stamps.
VERSION := $(shell sed -n '/CFBundleShortVersionString/{n;s/.*<string>\(.*\)<\/string>.*/\1/p;}' ../Info.plist)
DEFS   := -DMINICODE_VERSION='"$(VERSION)"'

ifeq ($(TERMINAL),1)
  PKGS += vte-2.91-gtk4
  DEFS += -DMINICODE_ENABLE_TERMINAL
endif
ifeq ($(BROWSER),1)
  PKGS += webkitgtk-6.0
  DEFS += -DMINICODE_ENABLE_BROWSER
endif
ifeq ($(PDF),1)
  PKGS += poppler-glib
  DEFS += -DMINICODE_ENABLE_PDF
endif
ifeq ($(WEBIMAGES),1)
  PKGS += libsoup-3.0
  DEFS += -DMINICODE_ENABLE_WEB_IMAGES
endif

# Lazily expanded (=, not :=) so `make test`, which needs no GTK, does not
# invoke pkg-config and complain about missing packages.
PKG_CFLAGS = $(shell pkg-config --cflags $(PKGS))
PKG_LIBS   = $(shell pkg-config --libs   $(PKGS))

INCLUDES := -Isrc -I../src

.PHONY: all run test clean
all: $(BIN)

$(BIN): $(SRCS)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $(DEFS) $(INCLUDES) $(PKG_CFLAGS) $(SRCS) \
	    $(PKG_LIBS) -o $(BIN)

DIR ?= .
run: $(BIN)
	$(BIN) $(DIR)

# Pure-C++ tests for the port: no GTK headers, no display, no libraries. These
# run anywhere g++ does, which is the only automated check available for a GUI
# this environment cannot drive.
TEST_BIN := $(BUILD)/run_tests
test:
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDES) tests/run_tests.cpp src/ThemeCss.cpp \
	    ../src/SyntaxHighlighter.cpp ../src/Settings.cpp ../src/LineComments.cpp \
	    src/PageWords.cpp ../src/LatexDoc.cpp ../src/SyncTex.cpp \
	    ../src/TermLinks.cpp src/TermLinkPath.cpp \
	    ../src/GitStatus.cpp ../src/GitGraph.cpp ../src/GitModel.cpp \
	    src/WebImageRules.cpp src/LaunchOptions.cpp \
	    -o $(TEST_BIN)
	@$(TEST_BIN)

clean:
	rm -rf $(BUILD)
