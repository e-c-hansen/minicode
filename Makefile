# MiniCode — native macOS editor. No third-party dependencies.
# Only Apple system frameworks + the C++ standard library.

APP      := MiniCode
BUNDLE   := $(APP).app
BIN      := build/$(APP)
SRC      := $(wildcard src/*.mm) $(wildcard src/*.cpp)

# clang++ on the Mac, where it is the only compiler that builds the app. On
# other systems (the Linux CI job runs `make test`) keep make's default, g++,
# unless CXX was given on the command line or in the environment.
ifeq ($(origin CXX),default)
  ifeq ($(shell uname -s),Darwin)
    CXX := clang++
  endif
endif
CXXFLAGS := -std=c++17 -fobjc-arc -Wall -Wextra -O2 -Isrc
LDFLAGS  := -framework Cocoa -framework WebKit -framework CoreServices \
            -framework Quartz -framework AVKit -framework AVFoundation \
            -framework CoreMedia -lz

# The pure-C++ core, testable on its own (no frameworks, no Objective-C).
CORE_SRC := src/SyntaxHighlighter.cpp src/MarkdownParser.cpp src/TerminalStream.cpp \
            src/TerminalScreen.cpp \
            src/Settings.cpp src/RunFile.cpp src/LineComments.cpp \
            src/LatexDoc.cpp src/SyncTex.cpp src/Json.cpp src/LspClient.cpp \
            src/TermLinks.cpp src/FolderSearch.cpp src/MarkdownEdit.cpp src/MarkdownTasks.cpp \
            src/GitStatus.cpp src/GitGraph.cpp src/MathTex.cpp src/EditorTabs.cpp \
            src/Formatter.cpp src/InstallHints.cpp src/LineDiff.cpp \
            src/PythonEnvs.cpp \
            src/AutoIndent.cpp src/KeepSorted.cpp src/FileDetails.cpp \
            src/ChangeRequest.cpp src/MultiCaret.cpp src/CommandPalette.cpp src/Backups.cpp

# Each source file compiles to its own object in build/obj (build/test-obj for
# the tests), with a dependency file listing the headers it read, so touching
# one file rebuilds that file and relinks. A change to the compiler or the
# flags rebuilds everything (build/obj/flags.txt keeps the last command).
OBJDIR   := build/obj
OBJ      := $(patsubst src/%,$(OBJDIR)/%.o,$(SRC))
TESTDIR  := build/test-obj
TESTFLAGS := -std=c++17 -Wall -Wextra -Isrc -pthread
TEST_OBJ := $(TESTDIR)/run_tests.cpp.o $(patsubst src/%,$(TESTDIR)/%.o,$(CORE_SRC))

# Parallel by default, one job per core: GNU make 3.81 (the Mac's) cannot tell
# a makefile whether -j was given, so `make JOBS=1` is the way to build one
# file at a time. Only the top-level make sets it (a sub-make shares its
# parent's jobs), and never alongside `clean`, which must finish first.
JOBS ?= $(shell getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
ifeq ($(MAKELEVEL),0)
  ifeq ($(filter clean,$(MAKECMDGOALS)),)
    MAKEFLAGS += -j$(JOBS)
  endif
endif

# The command each object was built with, rewritten only when it changes, so
# objects depend on the compiler and the flags. Skipped for a bare `make clean`.
APPCMD    := $(CXX) $(CXXFLAGS)
TESTCMD   := $(CXX) $(TESTFLAGS)
ifneq ($(if $(MAKECMDGOALS),$(filter-out clean,$(MAKECMDGOALS)),all),)
  $(shell mkdir -p $(OBJDIR) && printf '%s\n' '$(APPCMD)' | cmp -s - $(OBJDIR)/flags.txt || \
          printf '%s\n' '$(APPCMD)' > $(OBJDIR)/flags.txt)
  $(shell mkdir -p $(TESTDIR) && printf '%s\n' '$(TESTCMD)' | cmp -s - $(TESTDIR)/flags.txt || \
          printf '%s\n' '$(TESTCMD)' > $(TESTDIR)/flags.txt)
endif

.PHONY: all app run test dmg dist-zip clean demos membench icon

all: app

$(BIN): $(OBJ)
	$(CXX) $(CXXFLAGS) $(OBJ) $(LDFLAGS) -o $(BIN)

# After a `make clean` in the same run the folders and flags.txt are gone.
$(OBJDIR)/flags.txt:
	@mkdir -p $(@D) && printf '%s\n' '$(APPCMD)' > $@

$(OBJDIR)/%.mm.o: src/%.mm $(OBJDIR)/flags.txt
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(OBJDIR)/%.cpp.o: src/%.cpp $(OBJDIR)/flags.txt
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

# Assemble a minimal .app bundle so Cocoa treats it as a real GUI app.
app: $(BIN)
	@rm -rf $(BUNDLE)
	@mkdir -p $(BUNDLE)/Contents/MacOS $(BUNDLE)/Contents/Resources
	@cp $(BIN) $(BUNDLE)/Contents/MacOS/$(APP)
	@cp Info.plist $(BUNDLE)/Contents/Info.plist
	@echo "APPL????" > $(BUNDLE)/Contents/PkgInfo
	@cp resources/AppIcon.icns $(BUNDLE)/Contents/Resources/AppIcon.icns
	@cp scripts/minicode-launcher $(BUNDLE)/Contents/Resources/minicode
	@chmod +x $(BUNDLE)/Contents/Resources/minicode
	@# Ad-hoc signature (free, no Apple account). Required to run on Apple
	@# Silicon and keeps granted permissions stable across launches. It does
	@# NOT remove the Gatekeeper download warning — only notarization does.
	@codesign --force --sign - $(BUNDLE) 2>/dev/null || true
	@echo "Built $(BUNDLE)"

# Launch, opening the given folder (defaults to current directory).
run: app
	./$(BUNDLE)/Contents/MacOS/$(APP) $(or $(DIR),.)

# Unit-test the pure-C++ core (syntax highlighter, markdown parser, terminal
# output stream).
test: build/run_tests
	@./build/run_tests

build/run_tests: $(TEST_OBJ)
	$(CXX) $(TESTFLAGS) $(TEST_OBJ) -o $@

$(TESTDIR)/flags.txt:
	@mkdir -p $(@D) && printf '%s\n' '$(TESTCMD)' > $@

$(TESTDIR)/run_tests.cpp.o: tests/run_tests.cpp $(TESTDIR)/flags.txt
	@mkdir -p $(@D)
	$(CXX) $(TESTFLAGS) -MMD -MP -c $< -o $@

$(TESTDIR)/%.cpp.o: src/%.cpp $(TESTDIR)/flags.txt
	@mkdir -p $(@D)
	$(CXX) $(TESTFLAGS) -MMD -MP -c $< -o $@

# Regenerate the app icon (resources/AppIcon.icns) from tools/makeicon.m.
icon:
	@mkdir -p build
	@clang -framework Cocoa tools/makeicon.m -o build/makeicon
	@./build/makeicon /tmp/mc_icon.png
	@rm -rf /tmp/mc.iconset && mkdir -p /tmp/mc.iconset
	@for sz in 16 32 64 128 256 512; do \
		sips -z $$sz $$sz /tmp/mc_icon.png --out /tmp/mc.iconset/icon_$${sz}x$${sz}.png >/dev/null; \
		d=$$((sz*2)); \
		sips -z $$d $$d /tmp/mc_icon.png --out /tmp/mc.iconset/icon_$${sz}x$${sz}@2x.png >/dev/null; \
	done
	@cp /tmp/mc_icon.png /tmp/mc.iconset/icon_512x512@2x.png
	@iconutil -c icns /tmp/mc.iconset -o resources/AppIcon.icns
	@echo "Regenerated resources/AppIcon.icns"

# Record the README's demo GIFs (docs/demos/*.gif) by playing scripted scenes
# in the real app; see src/Demo.mm and scripts/demos.sh. Windows appear on
# screen while it runs. `make demos SCENES="tour latex"` records just those;
# POSTERS=1 also writes a still PNG of each.
build/makegif: tools/makegif.m
	@mkdir -p build
	clang -fobjc-arc -Wall -Wextra -O2 -framework Cocoa \
		-framework UniformTypeIdentifiers tools/makegif.m -o build/makegif

demos: app build/makegif
	POSTERS=$(POSTERS) ./scripts/demos.sh $(SCENES)

# Package the app into a distributable (unsigned) disk image. The script runs
# `make app` too, which finds the binary up to date.
dmg: app
	./scripts/make-dmg.sh

# Zip the app for a GitHub Release (preserves the bundle correctly).
dist-zip: app
	@ditto -c -k --sequesterRsrc --keepParent $(BUNDLE) $(APP).zip
	@echo "Created $(APP).zip"

clean:
	rm -rf build $(BUNDLE) $(APP).dmg $(APP).zip

# Memory used by MiniCode for a day-to-day workload, against VS Code plus
# Chrome plus Preview doing the same work; see scripts/membench.sh. Windows
# appear on screen while it runs, for a few minutes.
build/procmem: tools/procmem.c
	@mkdir -p build
	$(CC) -O2 -Wall -Wextra $< -o $@

membench: app build/procmem
	./scripts/membench.sh

-include $(OBJ:.o=.d) $(TEST_OBJ:.o=.d)
