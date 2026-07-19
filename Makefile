# MiniCode — native macOS editor. No third-party dependencies.
# Only Apple system frameworks + the C++ standard library.

APP      := MiniCode
BUNDLE   := $(APP).app
BIN      := build/$(APP)
SRC      := $(wildcard src/*.mm) $(wildcard src/*.cpp)

CXX      := clang++
CXXFLAGS := -std=c++17 -fobjc-arc -Wall -Wextra -O2 -Isrc
LDFLAGS  := -framework Cocoa -framework WebKit -framework CoreServices

# The pure-C++ core, testable on its own (no frameworks, no Objective-C).
CORE_SRC := src/SyntaxHighlighter.cpp src/MarkdownParser.cpp

.PHONY: all app run test dmg clean

all: app

$(BIN): $(SRC)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(SRC) $(LDFLAGS) -o $(BIN)

# Assemble a minimal .app bundle so Cocoa treats it as a real GUI app.
app: $(BIN)
	@rm -rf $(BUNDLE)
	@mkdir -p $(BUNDLE)/Contents/MacOS
	@cp $(BIN) $(BUNDLE)/Contents/MacOS/$(APP)
	@cp Info.plist $(BUNDLE)/Contents/Info.plist
	@echo "APPL????" > $(BUNDLE)/Contents/PkgInfo
	@echo "Built $(BUNDLE)"

# Launch, opening the given folder (defaults to current directory).
run: app
	./$(BUNDLE)/Contents/MacOS/$(APP) $(or $(DIR),.)

# Unit-test the pure-C++ core (syntax highlighter + markdown parser).
test:
	@mkdir -p build
	$(CXX) -std=c++17 -Wall -Wextra -Isrc tests/run_tests.cpp $(CORE_SRC) \
		-o build/run_tests
	@./build/run_tests

# Package the app into a distributable (unsigned) disk image.
dmg:
	./scripts/make-dmg.sh

clean:
	rm -rf build $(BUNDLE) $(APP).dmg
