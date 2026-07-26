# Plain Makefile fallback for people who don't want Meson.
#
#   make                     # core viewer: tree + editor + markdown (GTK4 only)
#   make TERMINAL=1          # add the VTE terminal panel
#   make BROWSER=1           # add the WebKitGTK browser panel
#   make TERMINAL=1 BROWSER=1
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

# Shared portable core (never copied — compiled straight from ../src).
CORE := ../src/SyntaxHighlighter.cpp ../src/MarkdownParser.cpp

# GUI sources. Terminal.cpp / Browser.cpp are always listed but compile to
# nothing unless their feature macro is defined.
GUI := src/main.cpp src/FileTree.cpp src/Editor.cpp src/Markdown.cpp \
       src/Terminal.cpp src/Browser.cpp

SRCS := $(GUI) $(CORE)

PKGS   := gtk4
DEFS   :=

ifeq ($(TERMINAL),1)
  PKGS += vte-2.91-gtk4
  DEFS += -DMINICODE_ENABLE_TERMINAL
endif
ifeq ($(BROWSER),1)
  PKGS += webkitgtk-6.0
  DEFS += -DMINICODE_ENABLE_BROWSER
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
	$(CXX) $(CXXFLAGS) $(INCLUDES) tests/run_tests.cpp \
	    ../src/SyntaxHighlighter.cpp -o $(TEST_BIN)
	@$(TEST_BIN)

clean:
	rm -rf $(BUILD)
