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
# The oldest macOS the build runs on, read from Info.plist's
# LSMinimumSystemVersion so the two cannot differ. Without it clang targets
# the macOS doing the build, and the binary refuses to launch on anything
# older. A newer API used without an @available check is an error, not a
# warning (scripts/check-minos.sh checks the built binaries).
ifeq ($(shell uname -s),Darwin)
  MACOS_MIN := $(shell sed -n '/<key>LSMinimumSystemVersion</{n;s/.*<string>\(.*\)<\/string>.*/\1/p;}' Info.plist)
  MINFLAGS  := -mmacosx-version-min=$(MACOS_MIN)
  # clang only: GCC (make test CXX=g++-16, as CI's Linux job) has no such flag.
  ifneq ($(findstring clang,$(shell $(CXX) --version 2>/dev/null | head -1)),)
    MINFLAGS += -Werror=unguarded-availability-new
  endif
endif
CXXFLAGS := -std=c++17 -fobjc-arc -Wall -Wextra -O2 -Isrc $(MINFLAGS)
LDFLAGS  := -framework Cocoa -framework WebKit -framework CoreServices \
            -framework Quartz -framework AVKit -framework AVFoundation \
            -framework CoreMedia -framework Carbon -lz -lsqlite3

# The pure-C++ core, testable on its own (no frameworks, no Objective-C).
CORE_SRC := src/SyntaxHighlighter.cpp src/MarkdownParser.cpp src/TerminalStream.cpp \
            src/TerminalScreen.cpp \
            src/Settings.cpp src/RunFile.cpp src/LineComments.cpp \
            src/LatexDoc.cpp src/SyncTex.cpp src/Json.cpp src/LspClient.cpp src/InlayHints.cpp \
            src/TermLinks.cpp src/FolderSearch.cpp src/TextFind.cpp src/Regex.cpp \
            src/MarkdownEdit.cpp src/MarkdownTasks.cpp \
            src/GitStatus.cpp src/GitGraph.cpp src/GitModel.cpp src/MathTex.cpp src/EditorTabs.cpp \
            src/Formatter.cpp src/InstallHints.cpp src/LineDiff.cpp src/QuickDiff.cpp \
            src/PythonEnvs.cpp \
            src/AutoIndent.cpp src/KeepSorted.cpp src/FileDetails.cpp \
            src/ChangeRequest.cpp src/MultiCaret.cpp src/AutoClose.cpp src/CommandPalette.cpp src/Backups.cpp \
            src/InlineAgent.cpp src/LineIndex.cpp src/BracketMatch.cpp src/BracketPairs.cpp \
            src/ControlProtocol.cpp src/SideBySide.cpp src/PreviewScroll.cpp \
            src/AgentTranscript.cpp src/AgentIndex.cpp src/AgentEvidence.cpp src/AgentView.cpp \
            src/SymbolOutline.cpp src/NavHistory.cpp \
            src/Folding.cpp \
            src/FileSniff.cpp src/Csv.cpp src/JsonOutline.cpp src/Notebook.cpp \
            src/Commands.cpp src/ProcessTable.cpp src/ClaudeHooks.cpp src/AttentionBoard.cpp \
            src/PageCheck.cpp src/IdeLink.cpp src/MermaidDiagram.cpp src/LatexScratch.cpp \
            src/LatexLog.cpp

# Each source file compiles to its own object in build/obj (build/test-obj for
# the tests), with a dependency file listing the headers it read, so touching
# one file rebuilds that file and relinks. A change to the compiler or the
# flags rebuilds everything (build/obj/flags.txt keeps the last command).
OBJDIR   := build/obj
OBJ      := $(patsubst src/%,$(OBJDIR)/%.o,$(SRC))

# A release build (`make dist-zip RELEASE=1`, as scripts/release.sh runs it)
# leaves out the demo scenes and the memory benchmark (src/Demo.mm,
# src/Bench.mm), which only `make demos` and `make membench` use; main.mm is
# compiled with MC_RELEASE and gets their stubs from Demo.h and Bench.h. Only
# main.mm and the link differ, so switching costs one file and a relink
# (build/obj/variant.txt says which was built last).
RELEASE_ONLY_OUT := $(OBJDIR)/Demo.mm.o $(OBJDIR)/Bench.mm.o
APP_OBJ  := $(if $(RELEASE),$(filter-out $(RELEASE_ONLY_OUT),$(OBJ)),$(OBJ))
VARIANT  := $(if $(RELEASE),release,development)
TESTDIR  := build/test-obj
TESTFLAGS := -std=c++17 -Wall -Wextra -Isrc -pthread $(MINFLAGS)
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
  $(shell printf '%s\n' '$(VARIANT)' | cmp -s - $(OBJDIR)/variant.txt || \
          printf '%s\n' '$(VARIANT)' > $(OBJDIR)/variant.txt)
endif

.PHONY: all app run test test-asan test-tsan guitest fakelsp install dmg dist-zip clean demos membench icon

all: app

$(BIN): $(APP_OBJ) $(OBJDIR)/variant.txt
	$(CXX) $(CXXFLAGS) $(APP_OBJ) $(LDFLAGS) -o $(BIN)

# The control channel's client (`minicode open`, `state`, ...): the launcher
# hands it every subcommand. Only the protocol and JSON from the core.
CLI     := build/minicode-cli
CLI_OBJ := $(OBJDIR)/tools/minicode-cli.cpp.o $(OBJDIR)/ControlProtocol.cpp.o $(OBJDIR)/Json.cpp.o \
           $(OBJDIR)/ProcessTable.cpp.o $(OBJDIR)/ClaudeHooks.cpp.o \
           $(OBJDIR)/PageCheck.cpp.o $(OBJDIR)/TermLinks.cpp.o

$(OBJDIR)/tools/%.cpp.o: tools/%.cpp $(OBJDIR)/flags.txt
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(CLI): $(CLI_OBJ)
	$(CXX) $(CXXFLAGS) $(CLI_OBJ) -o $@

# What BROWSER names in minicode's terminals (Terminal.mm): a script that
# is shell and JavaScript at once, beside the client (in the bundle it is
# in Resources, the client in MacOS; the GUI checks use build/'s copy).
BROWSER_HELPER := build/minicode-browser.js

$(BROWSER_HELPER): resources/minicode-browser.js
	@mkdir -p $(@D)
	@cp $< $@
	@chmod +x $@

# After a `make clean` in the same run the folders and flags.txt are gone.
$(OBJDIR)/flags.txt:
	@mkdir -p $(@D) && printf '%s\n' '$(APPCMD)' > $@

$(OBJDIR)/variant.txt:
	@mkdir -p $(@D) && printf '%s\n' '$(VARIANT)' > $@

$(OBJDIR)/main.mm.o: src/main.mm $(OBJDIR)/flags.txt $(OBJDIR)/variant.txt
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS)$(if $(RELEASE), -DMC_RELEASE) -MMD -MP -c $< -o $@

$(OBJDIR)/%.mm.o: src/%.mm $(OBJDIR)/flags.txt
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(OBJDIR)/%.cpp.o: src/%.cpp $(OBJDIR)/flags.txt
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

# The identity `make app` signs with: the local certificate when present.
SIGN_IDENTITY ?= $(shell security find-certificate -c "MiniCode Local Signing" >/dev/null 2>&1 && echo "MiniCode Local Signing")

# Assemble a minimal .app bundle so Cocoa treats it as a real GUI app.
# Only when something in it changed: the bundle's code signature is the
# target, so a `make` with nothing to do leaves the app, its signature and
# their dates alone (Request a Change takes a newer CodeResources for a
# fresh build).
APP_STAMP := $(BUNDLE)/Contents/_CodeSignature/CodeResources
APP_FILES := $(BIN) $(CLI) Info.plist resources/AppIcon.icns scripts/minicode-launcher \
             skills/minicode/SKILL.md resources/minicode-browser.js

app: $(APP_STAMP)

$(APP_STAMP): $(APP_FILES)
	@rm -rf $(BUNDLE)
	@mkdir -p $(BUNDLE)/Contents/MacOS $(BUNDLE)/Contents/Resources
	@cp $(BIN) $(BUNDLE)/Contents/MacOS/$(APP)
	@cp Info.plist $(BUNDLE)/Contents/Info.plist
	@echo "APPL????" > $(BUNDLE)/Contents/PkgInfo
	@cp resources/AppIcon.icns $(BUNDLE)/Contents/Resources/AppIcon.icns
	@cp scripts/minicode-launcher $(BUNDLE)/Contents/Resources/minicode
	@chmod +x $(BUNDLE)/Contents/Resources/minicode
	@cp resources/minicode-browser.js $(BUNDLE)/Contents/Resources/minicode-browser.js
	@chmod +x $(BUNDLE)/Contents/Resources/minicode-browser.js
	@# The Claude Code skill, for `minicode install-skill` (a Homebrew copy
	@# has no checkout to `make install` from).
	@mkdir -p $(BUNDLE)/Contents/Resources/skills/minicode
	@cp skills/minicode/SKILL.md $(BUNDLE)/Contents/Resources/skills/minicode/SKILL.md
	@cp $(CLI) $(BUNDLE)/Contents/MacOS/minicode-cli
	@# Signed with "MiniCode Local Signing" when the login keychain has it
	@# (scripts/make-signing-identity.sh makes it, once), so macOS keeps
	@# what the app was allowed (folders, the microphone) from one build to
	@# the next; an ad-hoc signature names the exact binary, and every build
	@# had to ask again. Ad-hoc otherwise, or if that signing fails. A
	@# signature is required to run on Apple Silicon; neither kind removes
	@# the Gatekeeper download warning, which only notarization does.
	@# The client in Contents/MacOS is signed first, as nested code must be.
	@if [ -n "$(SIGN_IDENTITY)" ] && \
	    codesign --force --sign "$(SIGN_IDENTITY)" $(BUNDLE)/Contents/MacOS/minicode-cli 2>/dev/null && \
	    codesign --force --sign "$(SIGN_IDENTITY)" $(BUNDLE) 2>/dev/null; then \
	    echo "Signed with $(SIGN_IDENTITY)"; \
	else \
	    codesign --force --sign - $(BUNDLE)/Contents/MacOS/minicode-cli 2>/dev/null || true; \
	    codesign --force --sign - $(BUNDLE) 2>/dev/null || true; \
	    echo "Signed ad hoc (scripts/make-signing-identity.sh keeps permissions across builds)"; \
	fi
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

# The same tests under AddressSanitizer and UndefinedBehaviorSanitizer, in
# build/test-asan (CI's Linux job runs it with g++). Undefined behaviour
# stops the run at the first report instead of printing and going on, so a
# report is a failure. The fuzz groups (tests/parser_fuzz.inc,
# tests/terminal_fuzz.inc) are where most of what it finds comes from.
ASANDIR   := build/test-asan
ASANFLAGS := $(TESTFLAGS) -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined \
             -fno-sanitize-recover=undefined
# GCC's -Wmaybe-uninitialized misfires under the sanitizers (in
# libstdc++'s <regex>, for TextFind), as GCC's manual warns it may; clang
# would warn about the option itself.
ifeq ($(findstring clang,$(shell $(CXX) --version 2>/dev/null | head -1)),)
  ASANFLAGS += -Wno-maybe-uninitialized
endif
ASAN_OBJ  := $(ASANDIR)/run_tests.cpp.o $(patsubst src/%,$(ASANDIR)/%.o,$(CORE_SRC))
ASANCMD   := $(CXX) $(ASANFLAGS)
ifneq ($(filter test-asan,$(MAKECMDGOALS)),)
  $(shell mkdir -p $(ASANDIR) && printf '%s\n' '$(ASANCMD)' | cmp -s - $(ASANDIR)/flags.txt || \
          printf '%s\n' '$(ASANCMD)' > $(ASANDIR)/flags.txt)
endif

test-asan: $(ASANDIR)/run_tests
	@./$(ASANDIR)/run_tests

$(ASANDIR)/run_tests: $(ASAN_OBJ)
	$(CXX) $(ASANFLAGS) $(ASAN_OBJ) -o $@

$(ASANDIR)/flags.txt:
	@mkdir -p $(@D) && printf '%s\n' '$(ASANCMD)' > $@

$(ASANDIR)/run_tests.cpp.o: tests/run_tests.cpp $(ASANDIR)/flags.txt
	@mkdir -p $(@D)
	$(CXX) $(ASANFLAGS) -MMD -MP -c $< -o $@

$(ASANDIR)/%.cpp.o: src/%.cpp $(ASANDIR)/flags.txt
	@mkdir -p $(@D)
	$(CXX) $(ASANFLAGS) -MMD -MP -c $< -o $@

# The same tests under ThreadSanitizer, in build/test-tsan (CI runs it on
# Linux with clang++-22: g++ 15's TSan build died there with a SEGV and no
# report; clang on the Mac works too). MC_PRINT_GROUPS=1 names each group
# on stderr as it starts, so a run that dies says where. The threaded
# parts of the core are Find in Folder's parallel walk, TextFind's regex
# worker, the language server's reader and the threading groups. A report
# stops the run (halt_on_error); one that is a known false positive goes in
# tests/tsan.supp, by name, with why. The timing bounds are skipped
# (MC_SKIP_TIMING, unless given): instrumented code costs several times the
# CPU the bounds were set for. The whole run took 4.5 minutes on an M3,
# building included.
TSANDIR   := build/test-tsan
TSANFLAGS := $(TESTFLAGS) -O1 -g -fno-omit-frame-pointer -fsanitize=thread
ifeq ($(findstring clang,$(shell $(CXX) --version 2>/dev/null | head -1)),)
  TSANFLAGS += -Wno-maybe-uninitialized
endif
TSAN_OBJ  := $(TSANDIR)/run_tests.cpp.o $(patsubst src/%,$(TSANDIR)/%.o,$(CORE_SRC))
TSANCMD   := $(CXX) $(TSANFLAGS)
ifneq ($(filter test-tsan,$(MAKECMDGOALS)),)
  $(shell mkdir -p $(TSANDIR) && printf '%s\n' '$(TSANCMD)' | cmp -s - $(TSANDIR)/flags.txt || \
          printf '%s\n' '$(TSANCMD)' > $(TSANDIR)/flags.txt)
endif

test-tsan: $(TSANDIR)/run_tests
	@MC_SKIP_TIMING="$${MC_SKIP_TIMING:-1}" \
	  TSAN_OPTIONS="halt_on_error=1 second_deadlock_stack=1 suppressions=$(CURDIR)/tests/tsan.supp $(TSAN_OPTIONS)" \
	  ./$(TSANDIR)/run_tests

$(TSANDIR)/run_tests: $(TSAN_OBJ)
	$(CXX) $(TSANFLAGS) $(TSAN_OBJ) -o $@

$(TSANDIR)/flags.txt:
	@mkdir -p $(@D) && printf '%s\n' '$(TSANCMD)' > $@

$(TSANDIR)/run_tests.cpp.o: tests/run_tests.cpp $(TSANDIR)/flags.txt
	@mkdir -p $(@D)
	$(CXX) $(TSANFLAGS) -MMD -MP -c $< -o $@

$(TSANDIR)/%.cpp.o: src/%.cpp $(TSANDIR)/flags.txt
	@mkdir -p $(@D)
	$(CXX) $(TSANFLAGS) -MMD -MP -c $< -o $@

# GUI checks (tests/gui, see tests/gui/README.md): the whole app, main.mm
# included as minicode_main, driven offscreen by a harness that never puts
# a window on screen or activates the app. `make guitest SUITES=control`
# runs one suite.
GUIDIR  := build/gui-obj
GUI_SRC := $(wildcard tests/gui/*.mm)
GUI_OBJ := $(patsubst tests/gui/%,$(GUIDIR)/%.o,$(GUI_SRC)) $(GUIDIR)/main.mm.o \
           $(filter-out $(OBJDIR)/main.mm.o,$(OBJ))

$(GUIDIR)/main.mm.o: src/main.mm $(OBJDIR)/flags.txt
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -Dmain=minicode_main -MMD -MP -c $< -o $@

$(GUIDIR)/%.mm.o: tests/gui/%.mm $(OBJDIR)/flags.txt
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -Itests/gui -MMD -MP -c $< -o $@

build/guitest: $(GUI_OBJ)
	$(CXX) $(CXXFLAGS) $(GUI_OBJ) $(LDFLAGS) -o $@

# A scripted language server (tests/fakelsp/fakelsp.cpp) for the GUI checks
# of language server features, so they run where no real server is
# installed (the CI runner). Plain C++ from the core's JSON and framing, built
# with the tests' flags, so it builds on Linux too.
FAKELSP     := build/fakelsp
FAKELSP_OBJ := $(TESTDIR)/fakelsp/fakelsp.cpp.o $(TESTDIR)/Json.cpp.o $(TESTDIR)/LspClient.cpp.o

$(TESTDIR)/fakelsp/%.cpp.o: tests/fakelsp/%.cpp $(TESTDIR)/flags.txt
	@mkdir -p $(@D)
	$(CXX) $(TESTFLAGS) -MMD -MP -c $< -o $@

$(FAKELSP): $(FAKELSP_OBJ)
	$(CXX) $(TESTFLAGS) $(FAKELSP_OBJ) -o $@

fakelsp: $(FAKELSP)

guitest: build/guitest $(CLI) $(FAKELSP) $(BROWSER_HELPER)
	@./build/guitest $(SUITES)

# The agent skill (skills/minicode) for Claude Code, in ~/.claude/skills,
# the same as `minicode install-skill` does from the app's bundle.
install:
	@bash scripts/minicode-launcher install-skill

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

# Package the app into a distributable (unsigned) disk image. The script
# builds it as a release (`make app RELEASE=1`).
dmg:
	./scripts/make-dmg.sh

# Zip the app for a GitHub Release (preserves the bundle correctly);
# scripts/release.sh runs `make dist-zip RELEASE=1`.
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

-include $(OBJ:.o=.d) $(TEST_OBJ:.o=.d) $(ASAN_OBJ:.o=.d) $(TSAN_OBJ:.o=.d) $(CLI_OBJ:.o=.d) $(GUI_OBJ:.o=.d) $(FAKELSP_OBJ:.o=.d)
